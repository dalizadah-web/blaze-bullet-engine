#include "blaze/uci/limits.h"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <string>
#include <vector>

namespace blaze {
namespace {

template <typename Integer>
bool parse_positive_or_zero(const std::string& text, Integer& value) {
    if (text.empty() || text.front() == '-') {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

}  // namespace

std::optional<GoParameters> parse_go(std::string_view arguments, std::string& error) {
    std::istringstream stream{std::string(arguments)};
    std::vector<std::string> tokens;
    for (std::string token; stream >> token;) {
        tokens.push_back(std::move(token));
    }

    GoParameters result;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const std::string& name = tokens[index];
        if (name == "infinite") {
            result.infinite = true;
            continue;
        }
        if (name == "ponder") {
            result.ponder = true;
            continue;
        }
        if (index + 1 >= tokens.size()) {
            error = "missing value for go " + name;
            return std::nullopt;
        }
        const std::string& value = tokens[++index];

        std::uint64_t parsed = 0;
        if (!parse_positive_or_zero(value, parsed)) {
            error = "invalid value for go " + name;
            return std::nullopt;
        }
        if (name == "wtime") result.white_time = std::chrono::milliseconds(parsed);
        else if (name == "btime") result.black_time = std::chrono::milliseconds(parsed);
        else if (name == "winc") result.white_increment = std::chrono::milliseconds(parsed);
        else if (name == "binc") result.black_increment = std::chrono::milliseconds(parsed);
        else if (name == "movetime") result.move_time = std::chrono::milliseconds(parsed);
        else if (name == "movestogo" && parsed > 0 && parsed <= 1000) result.moves_to_go = static_cast<int>(parsed);
        else if (name == "depth" && parsed > 0 && parsed <= 128) result.depth = static_cast<int>(parsed);
        else if (name == "nodes" && parsed > 0) result.nodes = parsed;
        else {
            error = "unsupported or out-of-range go parameter: " + name;
            return std::nullopt;
        }
    }
    error.clear();
    return result;
}

SearchLimits to_search_limits(const GoParameters& go, Color side_to_move) {
    SearchLimits limits;
    limits.depth = go.depth;
    limits.nodes = go.nodes;
    if (go.infinite || go.ponder) {
        return limits;
    }
    if (go.move_time.count() > 0) {
        limits.move_time = go.move_time;
        return limits;
    }

    const auto remaining = side_to_move == Color::White ? go.white_time : go.black_time;
    const auto increment = side_to_move == Color::White ? go.white_increment : go.black_increment;
    if (remaining.count() <= 0) {
        return limits;
    }

    const auto reserve = std::chrono::milliseconds(
        std::clamp<std::int64_t>(remaining.count() / 20, 5, 50));
    const auto usable = std::max(remaining - reserve, std::chrono::milliseconds(1));
    const int moves = go.moves_to_go > 0 ? go.moves_to_go : 20;
    auto budget = usable / moves + increment * 3 / 4;
    budget = std::min(budget, usable / 2);
    limits.move_time = std::max(budget, std::chrono::milliseconds(1));
    return limits;
}

}  // namespace blaze
