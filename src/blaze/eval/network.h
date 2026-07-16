#ifndef BLAZE_EVAL_NETWORK_H
#define BLAZE_EVAL_NETWORK_H

#include <cstdint>
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

class NetworkLoader final {
public:
    [[nodiscard]] static std::optional<Network> load(std::string_view path);
    [[nodiscard]] static std::optional<Network> load(
        std::string_view path,
        std::string& error);
};

}  // namespace blaze

#endif  // BLAZE_EVAL_NETWORK_H
