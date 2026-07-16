#ifndef BLAZE_SEARCH_SEARCH_H
#define BLAZE_SEARCH_SEARCH_H

#include "blaze/core/position.h"
#include "blaze/eval/classical.h"
#include "blaze/search/transposition_table.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

namespace blaze {

struct SearchLimits {
    int depth = 0;
    std::uint64_t nodes = 0;
    std::chrono::milliseconds move_time{0};
    int mate = 0;
    std::vector<Move> search_moves{};
};

struct SearchResult {
    Move best_move;
    int score = 0;
    int depth = 0;
    std::uint64_t nodes = 0;
    std::vector<Move> pv;
    bool stopped = false;
};

class Searcher {
public:
    explicit Searcher(TranspositionTable& table) : table_(table) {}

    [[nodiscard]] SearchResult search(
        Position position,
        const SearchLimits& limits,
        const std::atomic<bool>* external_stop = nullptr,
        const std::vector<std::uint64_t>& prior_keys = {});

private:
    struct Context {
        SearchLimits limits;
        const std::atomic<bool>* external_stop = nullptr;
        std::chrono::steady_clock::time_point start;
        std::uint64_t nodes = 0;
        bool stopped = false;
        std::vector<std::uint64_t> keys;
        std::vector<Move> root_moves;
    };

    TranspositionTable& table_;
    std::array<std::array<Move, 2>, 128> killers_{};
    std::array<std::array<std::array<int, 64>, 64>, 2> history_{};

    [[nodiscard]] int negamax(
        Position& position,
        int depth,
        int alpha,
        int beta,
        int ply,
        Context& context,
        std::vector<Move>& pv,
        bool allow_null = true);
    [[nodiscard]] int quiescence(
        Position& position,
        int alpha,
        int beta,
        int ply,
        Context& context,
        std::vector<Move>& pv);
    [[nodiscard]] bool should_stop(Context& context) const;
    [[nodiscard]] static bool is_repetition(const Context& context, std::uint64_t key);
};

}  // namespace blaze

#endif  // BLAZE_SEARCH_SEARCH_H
