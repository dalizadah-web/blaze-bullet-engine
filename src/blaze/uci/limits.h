#ifndef BLAZE_UCI_LIMITS_H
#define BLAZE_UCI_LIMITS_H

#include "blaze/core/types.h"
#include "blaze/search/search.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace blaze {

struct GoParameters {
    std::chrono::milliseconds white_time{0};
    std::chrono::milliseconds black_time{0};
    std::chrono::milliseconds white_increment{0};
    std::chrono::milliseconds black_increment{0};
    std::chrono::milliseconds move_time{0};
    int moves_to_go = 0;
    int depth = 0;
    std::uint64_t nodes = 0;
    bool infinite = false;
    bool ponder = false;
};

[[nodiscard]] std::optional<GoParameters> parse_go(
    std::string_view arguments,
    std::string& error);
[[nodiscard]] SearchLimits to_search_limits(const GoParameters& go, Color side_to_move);

}  // namespace blaze

#endif  // BLAZE_UCI_LIMITS_H
