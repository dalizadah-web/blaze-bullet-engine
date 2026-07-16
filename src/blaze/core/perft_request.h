#ifndef BLAZE_CORE_PERFT_REQUEST_H
#define BLAZE_CORE_PERFT_REQUEST_H

#include "blaze/core/position.h"

#include <optional>
#include <string_view>

namespace blaze {

struct PerftRequest {
    Position position;
    int depth = 0;
};

[[nodiscard]] std::optional<PerftRequest> parse_perft_request(std::string_view input);

}  // namespace blaze

#endif  // BLAZE_CORE_PERFT_REQUEST_H
