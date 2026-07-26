#include "blaze/eval/nnue_kernels.h"
#include "blaze/eval/network.h"
#if defined(BLAZE_NNUE_BENCHMARK)
#include <chrono>
#endif

#include <algorithm>
#include <cstdlib>

namespace blaze::nnue {
namespace {

#if defined(BLAZE_NNUE_BENCHMARK)
class KernelProfileScope final {
public:
    explicit KernelProfileScope(NnueProfileComponent component) : component_(component),
        sampled_(nnue_benchmark_should_sample_component(component)) {
        if (sampled_) start_ = std::chrono::steady_clock::now();
    }
    ~KernelProfileScope() {
        if (sampled_) {
            const auto elapsed = std::chrono::steady_clock::now() - start_;
            nnue_benchmark_record_component_sample(component_, static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
        }
    }
private:
    NnueProfileComponent component_;
    bool sampled_;
    std::chrono::steady_clock::time_point start_{};
};
#else
class KernelProfileScope final {
public:
    explicit KernelProfileScope(NnueProfileComponent) noexcept {}
};
#endif

constexpr int kWeightScaleBits = 6;
constexpr int kOutputScale = 16;
constexpr int kFirstLayerOutputs = 15;
constexpr int kFirstLayerTotalOutputs = kFirstLayerOutputs + 1;
constexpr int kHiddenOutputs = 32;

inline std::uint8_t clipped(std::int32_t value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value >> kWeightScaleBits, 0, 127));
}

inline std::uint8_t squared_clipped(std::int32_t value) noexcept {
    const auto square = static_cast<std::int64_t>(value) * value;
    return static_cast<std::uint8_t>(std::min<std::int64_t>(127, square >> 19));
}

void affine(const std::uint8_t* input,
            std::size_t input_dimensions,
            std::size_t output_dimensions,
            const std::int8_t* weights,
            const std::int32_t* biases,
            std::int32_t* output) noexcept {
    for (std::size_t row = 0; row < output_dimensions; ++row) {
        std::int32_t sum = biases[row];
        const std::int8_t* const weight_row = weights + row * input_dimensions;
        for (std::size_t column = 0; column < input_dimensions; ++column)
            sum += static_cast<std::int32_t>(weight_row[column]) * input[column];
        output[row] = sum;
    }
}

}  // namespace

void accumulate_add_scalar(std::int16_t* destination, const std::int16_t* weights) noexcept {
    for (std::size_t i = 0; i < kAccumulatorDimensions; ++i)
        destination[i] = static_cast<std::int16_t>(destination[i] + weights[i]);
}

void accumulate_subtract_scalar(std::int16_t* destination, const std::int16_t* weights) noexcept {
    for (std::size_t i = 0; i < kAccumulatorDimensions; ++i)
        destination[i] = static_cast<std::int16_t>(destination[i] - weights[i]);
}

void transform_scalar(const std::int16_t* pieces,
                      const std::int16_t* threats,
                      std::uint8_t* output) noexcept {
    for (std::size_t i = 0; i < kTransformedDimensions / 2; ++i) {
        const int first = std::clamp<int>(pieces[i] + threats[i], 0, 255);
        const int second = std::clamp<int>(pieces[i + kTransformedDimensions / 2] +
                                               threats[i + kTransformedDimensions / 2],
                                           0, 255);
        output[i] = static_cast<std::uint8_t>((first * second) / 512);
    }
}

std::int32_t propagate_scalar(const std::uint8_t* transformed,
                              const InferenceWeights& weights,
                              InferenceScratch& scratch) noexcept {
    {
        [[maybe_unused]] KernelProfileScope timer(NnueProfileComponent::FirstAffine);
        affine(transformed, kTransformedDimensions, kFirstLayerTotalOutputs,
               weights.fc0_weights, weights.fc0_biases, scratch.fc0.data());
    }

    {
        [[maybe_unused]] KernelProfileScope timer(NnueProfileComponent::Activation);
        for (int i = 0; i < kFirstLayerOutputs; ++i) {
            scratch.hidden[static_cast<std::size_t>(i)] = squared_clipped(scratch.fc0[static_cast<std::size_t>(i)]);
            scratch.hidden[static_cast<std::size_t>(i + kFirstLayerOutputs)] =
                clipped(scratch.fc0[static_cast<std::size_t>(i)]);
        }
        scratch.hidden[30] = 0;
        scratch.hidden[31] = 0;
    }

    {
        [[maybe_unused]] KernelProfileScope timer(NnueProfileComponent::HiddenAffine);
        affine(scratch.hidden.data(), kHiddenOutputs, kHiddenOutputs,
               weights.fc1_weights, weights.fc1_biases, scratch.fc1.data());
    }
    {
        [[maybe_unused]] KernelProfileScope timer(NnueProfileComponent::Activation);
        for (int i = 0; i < kHiddenOutputs; ++i)
            scratch.hidden[static_cast<std::size_t>(i)] = clipped(scratch.fc1[static_cast<std::size_t>(i)]);
    }

    std::int32_t output = weights.fc2_biases[0];
    {
        [[maybe_unused]] KernelProfileScope timer(NnueProfileComponent::OutputLayer);
        for (int i = 0; i < kHiddenOutputs; ++i)
            output += static_cast<std::int32_t>(weights.fc2_weights[i]) * scratch.hidden[static_cast<std::size_t>(i)];
    }

    const std::int32_t forward = scratch.fc0[kFirstLayerOutputs] * (600 * kOutputScale) /
                                 (127 * (1 << kWeightScaleBits));
    return output + forward;
}

KernelSet select_kernels(bool allow_avx2) noexcept {
    KernelSet kernels{
        accumulate_add_scalar,
        accumulate_subtract_scalar,
        transform_scalar,
        propagate_scalar,
        false};
#if (defined(__i386__) || defined(__x86_64__)) && (defined(__GNUC__) || defined(__clang__))
    const char* const force_scalar = std::getenv("BLAZE_NNUE_FORCE_SCALAR");
    __builtin_cpu_init();
    if (allow_avx2 && force_scalar == nullptr && __builtin_cpu_supports("avx2")) {
        kernels = KernelSet{
            accumulate_add_avx2,
            accumulate_subtract_avx2,
            transform_avx2,
            propagate_avx2,
            true};
    }
#endif
    return kernels;
}

}  // namespace blaze::nnue
