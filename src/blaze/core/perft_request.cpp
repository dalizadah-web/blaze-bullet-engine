#include "blaze/core/perft_request.h"

#include <charconv>

namespace blaze {

std::optional<PerftRequest> parse_perft_request(std::string_view input) {
    constexpr std::string_view utf8_bom{"\xEF\xBB\xBF", 3};
    if (input.starts_with(utf8_bom)) {
        input.remove_prefix(utf8_bom.size());
    }

    const std::size_t separator = input.rfind('\t');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view depth_text = input.substr(separator + 1);
    int depth = -1;
    const auto parsed_depth = std::from_chars(
        depth_text.data(),
        depth_text.data() + depth_text.size(),
        depth);
    auto position = Position::from_fen(input.substr(0, separator));
    if (!position.has_value() || parsed_depth.ec != std::errc{} ||
        parsed_depth.ptr != depth_text.data() + depth_text.size() || depth < 0) {
        return std::nullopt;
    }
    return PerftRequest{*position, depth};
}

}  // namespace blaze
