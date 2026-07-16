#ifndef BLAZE_EVAL_NETWORK_H
#define BLAZE_EVAL_NETWORK_H

#include "blaze/core/position.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace blaze {

struct Network {
    std::uint32_t version = 0;
    std::uint32_t features = 0;
    std::uint32_t hidden = 0;
    std::vector<std::uint8_t> weights;
};

class NetworkEvaluator final {
public:
    [[nodiscard]] static std::optional<NetworkEvaluator> create(
        const Network& network,
        std::string& error);

    [[nodiscard]] int evaluate(const Position& position) const;

private:
    static constexpr std::size_t feature_count = 768;
    static constexpr std::size_t hidden_count = 256;
    static constexpr std::size_t input_weight_bytes = feature_count * hidden_count * 2;
    static constexpr std::size_t hidden_bias_bytes = hidden_count * 4;
    static constexpr std::size_t output_weight_bytes = hidden_count * 2;
    static constexpr std::size_t output_bias_bytes = 4;
    static constexpr std::size_t expected_payload_bytes = input_weight_bytes +
        hidden_bias_bytes + output_weight_bytes + output_bias_bytes;

    std::vector<std::int16_t> input_weights_;
    std::array<std::int32_t, hidden_count> hidden_bias_{};
    std::array<std::int16_t, hidden_count> output_weights_{};
    std::int32_t output_bias_ = 0;
};

class NetworkLoader final {
public:
    [[nodiscard]] static std::optional<Network> load(std::string_view path);
    [[nodiscard]] static std::optional<Network> load(
        std::string_view path,
        std::string& error);
};

}  // namespace blaze

#endif  // BLAZE_EVAL_NETWORK_H
