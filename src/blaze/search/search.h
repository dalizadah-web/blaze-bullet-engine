#ifndef BLAZE_SEARCH_SEARCH_H
#define BLAZE_SEARCH_SEARCH_H

#include "blaze/core/position.h"
#include "blaze/eval/classical.h"
#include "blaze/eval/network.h"
#include "blaze/search/move_picker.h"
#include "blaze/search/pv_line.h"
#include "blaze/search/stack.h"
#include "blaze/search/time_manager.h"
#include "blaze/search/transposition_table.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace blaze {

struct SearchLimits {
    int depth = 0;
    std::uint64_t nodes = 0;
    std::chrono::milliseconds target_time{0};
    std::chrono::milliseconds move_time{0};
    int mate = 0;
    std::vector<Move> search_moves{};
    int threads = 1;
    SearchRegime regime = SearchRegime::Standard;
    int recommended_threads = 1;
    std::shared_ptr<std::atomic<std::uint64_t>> shared_node_budget{};
#ifndef NDEBUG
    int maximum_ply = 128;
    bool enable_probcut = true;
    bool enable_null_move = true;
#endif
};

struct SearchResult {
    Move best_move;
    int score = 0;
    int depth = 0;
    std::uint64_t nodes = 0;
#ifndef NDEBUG
    int maximum_extension_count = 0;
    int effective_maximum_ply = 128;
    std::uint64_t probcut_legal_checks = 0;
    std::uint64_t null_move_searches = 0;
    std::uint64_t null_move_pv_searches = 0;
    std::uint64_t null_move_verifications = 0;
#endif
    std::vector<Move> pv;
    bool stopped = false;
    // Instrumentation
    MovePicker::Stats picker_stats;
};

class Searcher {
public:
    explicit Searcher(TranspositionTable& table, const NetworkEvaluator* network = nullptr)
        : table_(table), network_(network) {}

    [[nodiscard]] SearchResult search(
        Position position,
        const SearchLimits& limits,
        const std::atomic<bool>* external_stop = nullptr,
        const std::vector<std::uint64_t>& prior_keys = {});
#ifndef NDEBUG
    [[nodiscard]] SearchResult debug_search_window(
        Position position,
        int depth,
        int alpha,
        int beta);
#endif

private:
    enum class NodeType : std::uint8_t { Root, PV, NonPV };

    struct EvalCacheEntry {
        std::uint64_t key = 0;
        int score = 0;
        bool valid = false;
    };

    struct Context {
        SearchLimits limits;
        const std::atomic<bool>* external_stop = nullptr;
        std::chrono::steady_clock::time_point start;
        std::uint64_t nodes = 0;
#ifndef NDEBUG
        int maximum_extension_count = 0;
        std::uint64_t probcut_legal_checks = 0;
        std::uint64_t null_move_searches = 0;
        std::uint64_t null_move_pv_searches = 0;
        std::uint64_t null_move_verifications = 0;
#endif
        bool stopped = false;
        std::uint64_t local_node_budget = 0;
        std::vector<std::uint64_t> keys;
        std::vector<Move> root_moves;
        std::array<SearchStackEntry, 132> stack{};
        MovePicker::Stats picker_stats;
        // Late move pruning counters
        std::uint64_t lmp_eligible = 0;
        std::uint64_t lmp_pruned = 0;
        std::array<std::uint64_t, 4> lmp_by_depth{};
    };

    TranspositionTable& table_;
    const NetworkEvaluator* network_ = nullptr;
    mutable std::array<EvalCacheEntry, 4096> eval_cache_{};
    std::array<std::array<Move, 64>, 64> countermoves_{};
    std::array<std::array<std::array<int, 64>, 64>, 2> history_{};

    [[nodiscard]] SearchResult search_parallel(
        Position position,
        const SearchLimits& limits,
        const std::atomic<bool>* external_stop,
        const std::vector<std::uint64_t>& prior_keys);

    [[nodiscard]] SearchResult search_window(
        Position position,
        const SearchLimits& limits,
        int depth,
        int ply,
        Move previous_move,
        int extension_count,
        int alpha,
        int beta,
        const std::atomic<bool>* external_stop,
        const std::vector<std::uint64_t>& prior_keys,
        std::chrono::steady_clock::time_point start);

    template<NodeType node_type>
    [[nodiscard]] int negamax(
        Position& position,
        int depth,
        int alpha,
        int beta,
        int ply,
        Context& context,
        PvLine& pv,
        bool allow_null = true);
    [[nodiscard]] int quiescence(
        Position& position,
        int alpha,
        int beta,
        int ply,
        Context& context,
        PvLine& pv);
    [[nodiscard]] bool should_stop(Context& context) const;
    [[nodiscard]] bool consume_node(Context& context) const;
    [[nodiscard]] int evaluate_position(const Position& position) const;
    [[nodiscard]] int maximum_ply_score(Position& position, int ply) const;
    [[nodiscard]] static bool is_repetition(const Context& context, std::uint64_t key);
};

}  // namespace blaze

#endif  // BLAZE_SEARCH_SEARCH_H
