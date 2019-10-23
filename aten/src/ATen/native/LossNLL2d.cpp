#include <ATen/ATen.h>
#include <ATen/AccumulateType.h>
#include <ATen/Dispatch.h>
#include <ATen/Parallel.h>
#include <ATen/TensorUtils.h>

namespace at {
namespace native {

namespace {

inline void check_inputs_nll_loss2d(
    const Tensor& input,
    const Tensor& target,
    const Tensor& weight) {
  TORCH_CHECK(
      target.dim() == 3,
      "only batches of spatial targets supported (3D tensors)"
      " but got targets of dimension: ",
      target.dim());
  TORCH_CHECK(
      input.dim() == 4,
      "only batches of spatial inputs supported (4D tensors), "
      "but got input of dimension: ",
      input.dim());
  TORCH_CHECK(
      !weight.defined() || weight.numel() == input.size(1),
      "weight tensor should be defined either for all or no classes");

  const int64_t input0 = input.size(0);
  const int64_t input2 = input.size(2);
  const int64_t input3 = input.size(3);
  const int64_t target0 = target.size(0);
  const int64_t target1 = target.size(1);
  const int64_t target2 = target.size(2);
  TORCH_CHECK(
      input0 == target0 && input2 == target1 && input3 == target2,
      "size mismatch (got input: ",
      input.sizes(),
      " , target: ",
      target.sizes());
}

inline void check_gradout_shape_nll_loss2d(
    const Tensor& grad_output,
    const Tensor& target) {
  TORCH_CHECK(
      grad_output.dim() == 3,
      "grad_output must have same dimension as target (3) but got dimension: ",
      grad_output.sizes());

  const int64_t grad_output0 = grad_output.size(0);
  const int64_t grad_output1 = grad_output.size(1);
  const int64_t grad_output2 = grad_output.size(2);
  const int64_t target0 = target.size(0);
  const int64_t target1 = target.size(1);
  const int64_t target2 = target.size(2);
  TORCH_CHECK(
      grad_output0 == target0 && grad_output1 == target1 &&
          grad_output2 == target2,
      "size mismatch (got grad_output: ",
      grad_output.sizes(),
      " target: ",
      target.sizes());
}

void nll_loss2d_forward_out_cpu_template(
    Tensor& output,
    Tensor& total_weight,
    const Tensor& input,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index) {
  check_inputs_nll_loss2d(input, target, weight);
  const int64_t n_classes = input.size(1);
  total_weight.resize_({1});

  AT_DISPATCH_FLOATING_TYPES_AND(
      ScalarType::BFloat16,
      input.scalar_type(),
      "nll_loss2d_forward_out_cpu_template",
      [&] {
        auto weight_contiguous = weight.contiguous();
        const scalar_t* weight_data =
            weight.defined() ? weight_contiguous.data_ptr<scalar_t>() : nullptr;

        // we check target indicies but cannot throw inside parallel_for
        std::atomic<int> invalid_target(-1);

        if (reduction == Reduction::None) {
          const int64_t batch_size = input.size(0);
          const int64_t H = input.size(2);
          const int64_t W = input.size(3);

          output.resize_({batch_size, H, W});
          auto input_acc = input.accessor<scalar_t, 4>();
          auto output_acc = output.accessor<scalar_t, 3>();
          auto target_acc = target.accessor<int64_t, 3>();

          at::parallel_for(0, batch_size, 0, [&](int64_t start, int64_t end) {
            for (int64_t b = start; b < end; b++) {
              for (int64_t h = 0; h < H; h++) {
                for (int64_t w = 0; w < W; w++) {
                  const int64_t cur_target = (int64_t)target_acc[b][h][w];

                  if (cur_target == ignore_index) {
                    output_acc[b][h][w] = static_cast<scalar_t>(0);
                    continue;
                  }
                  if (cur_target >= 0 && cur_target < n_classes) {
                    const scalar_t cur_weight = weight_data != nullptr
                        ? weight_data[cur_target]
                        : static_cast<scalar_t>(1);
                    output_acc[b][h][w] =
                        -input_acc[b][cur_target][h][w] * cur_weight;
                  } else {
                    int tmp = -1;
                    invalid_target.compare_exchange_strong(tmp, cur_target);
                  }
                }
              }
            }
          });

          TORCH_CHECK(
              invalid_target.load() < 0,
              "Target ",
              invalid_target.load(),
              " out of bounds.");

          return;
        }

        // produce scalar outputs for the reduction case
        output.resize_({});

        auto input_contiguous = input.contiguous();
        auto target_contiguous = target.contiguous();

        const scalar_t* input_data = input_contiguous.data_ptr<scalar_t>();
        const int64_t* target_data = target_contiguous.data_ptr<int64_t>();

        const int64_t batch_size = input.size(0);
        const int64_t map_size = input.size(2) * input.size(3);
        const int64_t sample_size = map_size * n_classes;

        const int64_t grain_size =
            std::max({1L,
                      at::internal::GRAIN_SIZE / map_size,
                      batch_size / get_num_threads()});
        auto result = at::parallel_reduce(
            0,
            batch_size,
            grain_size,
            std::make_tuple<scalar_t, scalar_t>(0, 0),
            [&](int64_t partial_begin,
                int64_t partial_end,
                std::tuple<scalar_t, scalar_t> identifiy) {
              scalar_t total_weight_val = 0;
              scalar_t output_val = 0;

              for (int64_t b = partial_begin; b < partial_end; b++) {
                for (int64_t elem = 0; elem < map_size; elem++) {
                  const int64_t cur_target = target_data[b * map_size + elem];
                  if (cur_target == ignore_index) {
                    continue;
                  }

                  if (cur_target >= 0 && cur_target < n_classes) {
                    const scalar_t weight_val = weight_data
                        ? weight_data[cur_target]
                        : static_cast<scalar_t>(1);
                    total_weight_val += weight_val;
                    output_val -=
                        input_data
                            [b * sample_size + cur_target * map_size + elem] *
                        weight_val;
                  } else {
                    int tmp = -1;
                    invalid_target.compare_exchange_strong(tmp, cur_target);
                  }
                }
              }
              return std::make_tuple(output_val, total_weight_val);
            },
            [](const std::tuple<scalar_t, scalar_t>& a,
               const std::tuple<scalar_t, scalar_t>& b) {
              return std::make_tuple(
                  std::get<0>(a) + std::get<0>(b),
                  std::get<1>(a) + std::get<1>(b));
            });

        TORCH_CHECK(
            invalid_target.load() < 0,
            "Target ",
            invalid_target.load(),
            " out of bounds.");

        scalar_t output_val = std::get<0>(result);
        const scalar_t total_weight_val = std::get<1>(result);

        if (reduction == Reduction::Mean &&
            (total_weight_val != 0 || input.numel() == 0)) {
          // allow NaN result for total_weight_val == 0 case, see #15870
          output_val /= total_weight_val;
        }

        *total_weight.data_ptr<scalar_t>() = total_weight_val;
        *output.data_ptr<scalar_t>() = output_val;
      });
} // namespace

void nll_loss2d_backward_out_cpu_template(
    Tensor& grad_input,
    const Tensor& grad_output,
    const Tensor& input,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index,
    const Tensor& total_weight) {
  check_inputs_nll_loss2d(input, target, weight);

  grad_input.resize_as_(input);
  grad_input.zero_();

  TORCH_CHECK(grad_input.is_contiguous(), "grad_input must be contiguous");

  AT_DISPATCH_FLOATING_TYPES_AND(
      ScalarType::BFloat16,
      input.scalar_type(),
      "nll_loss2d_backward_out_cpu_template",
      [&] {
        auto weight_contiguous = weight.contiguous();
        const scalar_t* weight_data =
            weight.defined() ? weight_contiguous.data_ptr<scalar_t>() : nullptr;

        if (reduction == at::Reduction::None) {
          check_gradout_shape_nll_loss2d(grad_output, target);

          const int64_t batch_size = input.size(0);
          const int64_t H = input.size(2);
          const int64_t W = input.size(3);

          auto grad_input_acc = grad_input.accessor<scalar_t, 4>();
          auto grad_output_acc = grad_output.accessor<scalar_t, 3>();
          auto target_acc = target.accessor<int64_t, 3>();

          at::parallel_for(0, batch_size, 0, [&](int64_t start, int64_t end) {
            for (int64_t b = start; b < end; b++) {
              for (int64_t h = 0; h < H; h++) {
                for (int64_t w = 0; w < W; w++) {
                  const int64_t cur_target = target_acc[b][h][w];
                  if (cur_target == ignore_index) {
                    continue;
                  }
                  const scalar_t value =
                      -(weight_data ? weight_data[cur_target]
                                    : static_cast<scalar_t>(1));
                  const scalar_t grad_output_value = grad_output_acc[b][h][w];
                  grad_input_acc[b][cur_target][h][w] =
                      value * grad_output_value;
                }
              }
            }
          });
          return;
        }

        const scalar_t total_weight_value =
            total_weight.accessor<scalar_t, 1>()[0];
        if (total_weight_value <= 0) {
          return;
        }

        TORCH_CHECK(
            grad_output.dim() <= 1 && grad_output.numel() == 1,
            "Expected a single element grad_output tensor, but got: ",
            grad_output.sizes());

        const scalar_t grad_output_value = *grad_output.data_ptr<scalar_t>();

        const auto target_contiguous = target.contiguous();
        const int64_t* target_data = target_contiguous.data_ptr<int64_t>();

        scalar_t* grad_input_data = grad_input.data_ptr<scalar_t>();

        const int64_t batch_size = input.size(0);
        const int64_t n_classes = input.size(1);
        const int64_t map_size = input.size(2) * input.size(3);
        const int64_t sample_size = map_size * n_classes;

        scalar_t normalize = (reduction == at::Reduction::Mean)
            ? total_weight_value
            : static_cast<scalar_t>(1);

        at::parallel_for(0, batch_size, 0, [&](int64_t start, int64_t end) {
          for (int64_t b = start; b < end; b++) {
            for (int64_t elem = 0; elem < map_size; elem++) {
              const int64_t cur_target = target_data[b * map_size + elem];
              if (cur_target == ignore_index) {
                continue;
              }

              TORCH_CHECK(cur_target >= 0 && cur_target < n_classes);

              const int64_t index =
                  b * sample_size + cur_target * map_size + elem;
              const scalar_t w = weight_data != nullptr
                  ? weight_data[cur_target]
                  : static_cast<scalar_t>(1);
              grad_input_data[index] = -w / normalize * grad_output_value;
            }
          }
        });
      });
}

} // namespace

std::tuple<Tensor&, Tensor&> nll_loss2d_forward_out_cpu(
    Tensor& output,
    Tensor& total_weight,
    const Tensor& self,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index) {
  nll_loss2d_forward_out_cpu_template(
      output, total_weight, self, target, weight, reduction, ignore_index);
  return std::tuple<Tensor&, Tensor&>(output, total_weight);
}

std::tuple<Tensor, Tensor> nll_loss2d_forward_cpu(
    const Tensor& self,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index) {
  auto output = at::empty({0}, self.options());
  auto total_weight = at::empty({0}, self.options());
  nll_loss2d_forward_out_cpu(
      output, total_weight, self, target, weight, reduction, ignore_index);
  return std::make_tuple(output, total_weight);
}

Tensor& nll_loss2d_backward_out_cpu(
    Tensor& grad_input,
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index,
    const Tensor& total_weight) {
  nll_loss2d_backward_out_cpu_template(
      grad_input,
      grad_output,
      self,
      target,
      weight,
      reduction,
      ignore_index,
      total_weight);
  return grad_input;
}

Tensor nll_loss2d_backward_cpu(
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index,
    const Tensor& total_weight) {
  auto grad_input = at::zeros_like(self);
  nll_loss2d_backward_out_cpu(
      grad_input,
      grad_output,
      self,
      target,
      weight,
      reduction,
      ignore_index,
      total_weight);
  return grad_input;
}

} // namespace native
} // namespace at
