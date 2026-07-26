#ifndef BLAZE_EVAL_NNUE_KERNELS_H
#define BLAZE_EVAL_NNUE_KERNELS_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace blaze::nnue {

constexpr std::size_t kAccumulatorDimensions = 1024;
constexpr std::size_t kTransformedDimensions = 1024;

struct alignas(64) InferenceScratch {
    std::array<std::int32_t, 32> fc0{};
    std::array<std::int32_t, 32> fc1{};
    std::array<std::uint8_t, 32> hidden{};
};

struct InferenceWeights {
    const std::int32_t* fc0_biases = nullptr;
    const std::int8_t* fc0_weights = nullptr;
    const std::int32_t* fc1_biases = nullptr;
    const std::int8_t* fc1_weights = nullptr;
    const std::int32_t* fc2_biases = nullptr;
    const std::int8_t* fc2_weights = nullptr;
};

using AccumulateKernel = void (*)(std::int16_t* destination, const std::int16_t* weights) noexcept;
using TransformKernel = void (*)(const std::int16_t* pieces,
                                 const std::int16_t* threats,
                                 std::uint8_t* output) noexcept;
using PropagateKernel = std::int32_t (*)(const std::uint8_t* transformed,
                                         const InferenceWeights& weights,
                                         InferenceScratch& scratch) noexcept;

struct KernelSet {
    AccumulateKernel add = nullptr;
    AccumulateKernel subtract = nullptr;
    TransformKernel transform = nullptr;
    PropagateKernel propagate = nullptr;
    bool avx2 = false;
};

void accumulate_add_scalar(std::int16_t* destination, const std::int16_t* weights) noexcept;
void accumulate_subtract_scalar(std::int16_t* destination, const std::int16_t* weights) noexcept;
void transform_scalar(const std::int16_t* pieces,
                      const std::int16_t* threats,
                      std::uint8_t* output) noexcept;
std::int32_t propagate_scalar(const std::uint8_t* transformed,
                              const InferenceWeights& weights,
                              InferenceScratch& scratch) noexcept;

void accumulate_add_avx2(std::int16_t* destination, const std::int16_t* weights) noexcept;
void accumulate_subtract_avx2(std::int16_t* destination, const std::int16_t* weights) noexcept;
void transform_avx2(const std::int16_t* pieces,
                    const std::int16_t* threats,
                    std::uint8_t* output) noexcept;
std::int32_t propagate_avx2(const std::uint8_t* transformed,
                            const InferenceWeights& weights,
                            InferenceScratch& scratch) noexcept;

[[nodiscard]] KernelSet select_kernels(bool allow_avx2 = true) noexcept;

}  // namespace blaze::nnue

#endif  // BLAZE_EVAL_NNUE_KERNELS_H
