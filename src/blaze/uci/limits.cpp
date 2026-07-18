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

bool is_go_keyword(const std::string& token) {
    return token == "wtime" || token == "btime" || token == "winc" ||
        token == "binc" || token == "movetime" || token == "movestogo" ||
        token == "depth" || token == "nodes" || token == "mate" ||
        token == "searchmoves" || token == "infinite" || token == "ponder";
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
        if (name == "searchmoves") {
            if (index + 1 >= tokens.size() || is_go_keyword(tokens[index + 1])) {
                error = "searchmoves requires at least one move";
                return std::nullopt;
            }
            while (index + 1 < tokens.size() && !is_go_keyword(tokens[index + 1])) {
                const std::string& move = tokens[++index];
                if (!move_from_uci(move)) {
                    error = "invalid searchmoves move: " + move;
                    return std::nullopt;
                }
                result.search_moves.push_back(move);
            }
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
        if (name == "wtime") {
            result.white_time = std::chrono::milliseconds(parsed);
            result.white_time_set = true;
        }
        else if (name == "btime") {
            result.black_time = std::chrono::milliseconds(parsed);
            result.black_time_set = true;
        }
        else if (name == "winc") result.white_increment = std::chrono::milliseconds(parsed);
        else if (name == "binc") result.black_increment = std::chrono::milliseconds(parsed);
        else if (name == "movetime") result.move_time = std::chrono::milliseconds(parsed);
        else if (name == "movestogo" && parsed > 0 && parsed <= 1000) result.moves_to_go = static_cast<int>(parsed);
        else if (name == "depth" && parsed > 0 && parsed <= 128) result.depth = static_cast<int>(parsed);
        else if (name == "nodes" && parsed > 0) result.nodes = parsed;
        else if (name == "mate" && parsed > 0 && parsed <= 64) result.mate = static_cast<int>(parsed);
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
    limits.mate = go.mate;
    limits.search_moves.reserve(go.search_moves.size());
    for (const std::string& move : go.search_moves) {
        limits.search_moves.push_back(*move_from_uci(move));
    }
    if (limits.depth == 0 && limits.mate > 0) {
        limits.depth = limits.mate * 2;
    }
    if (go.infinite || go.ponder) {
        return limits;
    }
    if (go.move_time.count() > 0) {
        limits.move_time = go.move_time;
        return limits;
    }

    const auto remaining = side_to_move == Color::White ? go.white_time : go.black_time;
    const auto increment = side_to_move == Color::White ? go.white_increment : go.black_increment;
    const bool clock_time_set = side_to_move == Color::White ? go.white_time_set : go.black_time_set;
    if (remaining.count() <= 0) {
        if (clock_time_set) limits.move_time = std::chrono::milliseconds(1);
        return limits;
    }

    const auto reserve = std::chrono::milliseconds(
        std::clamp<std::int64_t>(remaining.count() / 20, 5, 50));
    const auto usable = std::max(remaining - reserve, std::chrono::milliseconds(1));
    const int moves = go.moves_to_go > 0 ? go.moves_to_go : 12;
    auto budget = usable / moves + increment * 3 / 4;
    budget = std::min(budget, usable / 2);
    limits.move_time = std::max(budget, std::chrono::milliseconds(1));
    return limits;
}

}  // namespace blaze
