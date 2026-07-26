#include "blaze/eval/nnue_kernels.h"
#include "blaze/eval/network.h"
#if defined(BLAZE_NNUE_BENCHMARK)
#include <chrono>
#endif

#if defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>

#include <algorithm>

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

inline int horizontal_sum(__m256i value) noexcept {
    const __m128i halves = _mm_add_epi32(_mm256_castsi256_si128(value),
                                         _mm256_extracti128_si256(value, 1));
    __m128i sum = _mm_add_epi32(halves, _mm_shuffle_epi32(halves, 0x4e));
    sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0xb1));
    return _mm_cvtsi128_si32(sum);
}

inline int dot16(const std::uint8_t* input, const std::int8_t* weights) noexcept {
    const __m128i input8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
    const __m128i weight8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(weights));
    const __m256i input16 = _mm256_cvtepu8_epi16(input8);
    const __m256i weight16 = _mm256_cvtepi8_epi16(weight8);
    return horizontal_sum(_mm256_madd_epi16(input16, weight16));
}

inline std::uint8_t clipped(std::int32_t value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value >> kWeightScaleBits, 0, 127));
}

inline std::uint8_t squared_clipped(std::int32_t value) noexcept {
    const auto square = static_cast<std::int64_t>(value) * value;
    return static_cast<std::uint8_t>(std::min<std::int64_t>(127, square >> 19));
}

void affine_avx2(const std::uint8_t* input,
                 std::size_t input_dimensions,
                 std::size_t output_dimensions,
                 const std::int8_t* weights,
                 const std::int32_t* biases,
                 std::int32_t* output) noexcept {
    for (std::size_t row = 0; row < output_dimensions; ++row) {
        const std::int8_t* const weight_row = weights + row * input_dimensions;
        std::int32_t sum = biases[row];
        for (std::size_t column = 0; column < input_dimensions; column += 16)
            sum += dot16(input + column, weight_row + column);
        output[row] = sum;
    }
}

}  // namespace

void accumulate_add_avx2(std::int16_t* destination, const std::int16_t* weights) noexcept {
    for (std::size_t i = 0; i < kAccumulatorDimensions; i += 16) {
        const __m256i old_value = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(destination + i));
        const __m256i delta = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + i), _mm256_add_epi16(old_value, delta));
    }
}

void accumulate_subtract_avx2(std::int16_t* destination, const std::int16_t* weights) noexcept {
    for (std::size_t i = 0; i < kAccumulatorDimensions; i += 16) {
        const __m256i old_value = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(destination + i));
        const __m256i delta = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + i), _mm256_sub_epi16(old_value, delta));
    }
}

void transform_avx2(const std::int16_t* pieces,
                    const std::int16_t* threats,
                    std::uint8_t* output) noexcept {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i maximum = _mm256_set1_epi16(255);
    alignas(32) std::array<std::uint16_t, 16> products{};
    for (std::size_t i = 0; i < kTransformedDimensions / 2; i += 16) {
        __m256i first = _mm256_add_epi16(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pieces + i)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(threats + i)));
        __m256i second = _mm256_add_epi16(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pieces + i + kTransformedDimensions / 2)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(threats + i + kTransformedDimensions / 2)));
        first = _mm256_min_epi16(_mm256_max_epi16(first, zero), maximum);
        second = _mm256_min_epi16(_mm256_max_epi16(second, zero), maximum);
        _mm256_store_si256(reinterpret_cast<__m256i*>(products.data()), _mm256_mullo_epi16(first, second));
        for (std::size_t lane = 0; lane < products.size(); ++lane)
            output[i + lane] = static_cast<std::uint8_t>(products[lane] >> 9);
    }
}

std::int32_t propagate_avx2(const std::uint8_t* transformed,
                            const InferenceWeights& weights,
                            InferenceScratch& scratch) noexcept {
    {
        [[maybe_unused]] KernelProfileScope timer(NnueProfileComponent::FirstAffine);
        affine_avx2(transformed, kTransformedDimensions, kFirstLayerTotalOutputs,
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
        affine_avx2(scratch.hidden.data(), kHiddenOutputs, kHiddenOutputs,
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
        output += dot16(scratch.hidden.data(), weights.fc2_weights) +
            dot16(scratch.hidden.data() + 16, weights.fc2_weights + 16);
    }
    const std::int32_t forward = scratch.fc0[kFirstLayerOutputs] * (600 * kOutputScale) /
                                 (127 * (1 << kWeightScaleBits));
    return output + forward;
}

}  // namespace blaze::nnue

#else

namespace blaze::nnue {
void accumulate_add_avx2(std::int16_t* destination, const std::int16_t* weights) noexcept {
    accumulate_add_scalar(destination, weights);
}
void accumulate_subtract_avx2(std::int16_t* destination, const std::int16_t* weights) noexcept {
    accumulate_subtract_scalar(destination, weights);
}
void transform_avx2(const std::int16_t* pieces, const std::int16_t* threats, std::uint8_t* output) noexcept {
    transform_scalar(pieces, threats, output);
}
std::int32_t propagate_avx2(const std::uint8_t* transformed,
                            const InferenceWeights& weights,
                            InferenceScratch& scratch) noexcept {
    return propagate_scalar(transformed, weights, scratch);
}
}  // namespace blaze::nnue

#endif
