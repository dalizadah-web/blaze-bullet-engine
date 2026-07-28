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
#include <optional>
#include <mutex>
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
    int recommended_threads = 0;
    SearchTelemetry telemetry{};
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

    static constexpr std::size_t correction_table_size = 16384;
    static constexpr std::size_t continuation_correction_size = 4096;
    using MainHistory = std::array<std::array<int, 64>, 64>;
    using CaptureHistory = std::array<std::array<std::array<int, 64>, 7>, 7>;

    struct CorrectionHistories {
        std::array<std::array<int, correction_table_size>, 2> pawn{};
        std::array<std::array<int, correction_table_size>, 2> minor{};
        std::array<std::array<int, correction_table_size>, 2> major{};
        std::array<std::array<int, correction_table_size>, 2> non_pawn{};
        std::array<std::array<int, continuation_correction_size>, 2> continuation{};
        unsigned searches = 0;
    };

    struct RootMoveState {
        Move move;
        int score = -search_mate_score - 1;
        std::uint64_t effort = 0;
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
        std::vector<std::uint64_t> keys;
        std::vector<Move> root_moves;
        std::array<SearchStackEntry, 132> stack{};
        NnueThreadState* nnue = nullptr;
        unsigned worker_id = 0;
        bool restricted_root = false;
        MovePicker::Stats picker_stats;
    };

    TranspositionTable& table_;
    const NetworkEvaluator* network_ = nullptr;
    mutable std::array<EvalCacheEntry, 4096> eval_cache_{};
    std::array<std::array<Move, 64>, 64> countermoves_{};
    std::array<std::array<std::array<int, 64>, 64>, 2> history_{};
    CaptureHistory capture_history_{};
    MainHistory pawn_history_{};
    MainHistory continuation_history_{};
    MainHistory low_ply_history_{};
    std::array<std::array<int, 64>, 7> per_piece_history_{};
    MainHistory threat_history_{};
    CorrectionHistories correction_history_{};
    std::vector<RootMoveState> root_state_{};
    Move previous_root_move_{};
    int root_stability_ = 0;
    std::optional<NnueThreadState> worker_nnue_{};
    std::vector<std::unique_ptr<Searcher>> workers_{};
    std::mutex search_mutex_{};

    [[nodiscard]] SearchResult search_parallel(
        Position position,
        const SearchLimits& limits,
        const std::atomic<bool>* external_stop,
        const std::vector<std::uint64_t>& prior_keys,
        std::chrono::steady_clock::time_point start);
    [[nodiscard]] SearchResult search_single(
        Position position,
        const SearchLimits& limits,
        const std::atomic<bool>* external_stop,
        const std::vector<std::uint64_t>& prior_keys,
        std::chrono::steady_clock::time_point start,
        unsigned worker_id,
        bool bump_generation);

    [[nodiscard]] SearchResult search_window(
        Position position,
        const SearchLimits& limits,
        int depth,
        int ply,
        Move previous_move,
        int extension_count,
        int alpha,
        int beta,
        NnueThreadState* nnue,
        const std::atomic<bool>* external_stop,
        const std::vector<std::uint64_t>& prior_keys,
        std::chrono::steady_clock::time_point start);
    [[nodiscard]] NnueThreadState* prepare_nnue(const Position& position);
    void reset_task_heuristics();
    void update_history_tables(const Position& position, Move move, Move previous_move,
                               int depth, int ply);
    void update_history_penalty(const Position& position, Move move,
                                Move previous_move, int depth);
    [[nodiscard]] int quiet_history_score(
        const Position& position, Move move, Move previous_move, int ply) const;
    [[nodiscard]] int correction_value(const Position& position, Move previous_move) const;
    void update_correction(const Position& position, Move previous_move,
                           int raw_eval, int searched_value, int depth);
    void age_histories();

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
    [[nodiscard]] int evaluate_position(const Position& position, Context& context) const;
    [[nodiscard]] int maximum_ply_score(Position& position, int ply, Context& context) const;
    [[nodiscard]] static bool is_repetition(const Context& context, std::uint64_t key);
};

}  // namespace blaze

#endif  // BLAZE_SEARCH_SEARCH_H
