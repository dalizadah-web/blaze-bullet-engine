#include "blaze/search/search.h"

#include "blaze/core/movegen.h"
#include "test_support.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace blaze {
namespace {

Position position(std::string_view fen) {
    const auto parsed = Position::from_fen(fen);
    CHECK(parsed.has_value());
    return *parsed;
}

TEST_CASE(search_finds_and_proves_mate_in_one) {
    Position root = position("7k/5Q2/6K1/8/8/8/8/8 w - - 0 1");
    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 3});
    CHECK(result.best_move.is_valid());
    CHECK(result.score >= search_mate_threshold);
    StateInfo state;
    CHECK(root.make_move(result.best_move, state));
    MoveList replies;
    generate_legal(root, replies);
    CHECK(replies.empty());
    CHECK(in_check(root));
}

TEST_CASE(search_scores_root_checkmate_and_stalemate) {
    TranspositionTable table(1);
    Searcher searcher(table);
    Position mate = position("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1");
    Position stale = position("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    const SearchResult mate_result = searcher.search(mate, SearchLimits{.depth = 2});
    const SearchResult stale_result = searcher.search(stale, SearchLimits{.depth = 2});
    CHECK(!mate_result.best_move.is_valid());
    CHECK_EQ(mate_result.score, -search_mate_score);
    CHECK(!stale_result.best_move.is_valid());
    CHECK_EQ(stale_result.score, 0);
}

TEST_CASE(check_and_recapture_extensions_respect_the_path_budget) {
    Position root = position("6k1/5ppp/8/8/8/8/5PPP/3Q2K1 w - - 0 1");
    TranspositionTable table(8);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 10});
    CHECK(result.maximum_extension_count <= 2);
    CHECK(root.is_legal(result.best_move));
}

TEST_CASE(full_search_maximum_ply_checked_nonmate_uses_bounded_fallback) {
    Position root = position("6k1/8/8/8/8/8/8/3Q2K1 w - - 0 1");
    TranspositionTable table(1);
    Searcher searcher(table);
    SearchLimits limits{.depth = 4};
    limits.maximum_ply = 1;
    limits.search_moves = {Move(Square::D1, Square::D8)};

    Position checked = root;
    StateInfo state;
    CHECK(checked.make_move(limits.search_moves.front(), state));
    CHECK(in_check(checked));
    MoveList evasions;
    generate_legal(checked, evasions);
    CHECK(!evasions.empty());
    CHECK(evaluate(checked) != 0);

    const SearchResult result = searcher.search(root, limits);
    CHECK_EQ(result.score, 0);
    CHECK_EQ(result.maximum_extension_count, 1);
    CHECK(root.is_legal(result.best_move));
}

TEST_CASE(checking_recapture_adds_one_extension_and_path_caps_at_two) {
    Position root = position("5rkr/K4p2/8/8/8/5Q2/8/5R2 w - - 0 1");
    TranspositionTable table(1);
    Searcher searcher(table);
    SearchLimits limits{.depth = 4};
    limits.search_moves = {Move(Square::F3, Square::F7, MoveFlag::Capture)};
    const SearchResult result = searcher.search(root, limits);
    CHECK_EQ(result.maximum_extension_count, 2);
    CHECK(root.is_legal(result.best_move));
}

TEST_CASE(maximum_ply_checked_position_is_not_scored_as_ordinary_static_eval) {
    Position root = position("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    TranspositionTable table(1);
    Searcher searcher(table);
    SearchLimits limits{.depth = 4};
    limits.maximum_ply = 1;
    const SearchResult result = searcher.search(root, limits);
    CHECK_EQ(result.score, search_mate_score - 1);
}

TEST_CASE(qsearch_maximum_ply_checked_position_preserves_mate_distance) {
    Position root = position("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    TranspositionTable table(1);
    Searcher searcher(table);
    SearchLimits limits{.depth = 1};
    limits.maximum_ply = 1;
    limits.search_moves = {Move(Square::G6, Square::G7)};
    const SearchResult result = searcher.search(root, limits);
    CHECK_EQ(result.score, search_mate_score - 1);
}

TEST_CASE(debug_maximum_ply_is_clamped_to_the_safe_range) {
    Position root = position("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    TranspositionTable table(1);
    Searcher searcher(table);
    SearchLimits limits{.depth = 1};
    limits.maximum_ply = 0;
    limits.search_moves = {Move(Square::G6, Square::G7)};
    const SearchResult result = searcher.search(root, limits);
    CHECK_EQ(result.score, search_mate_score - 1);
}

TEST_CASE(debug_maximum_ply_upper_bound_is_clamped_to_stack_capacity) {
    Position root = position("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    TranspositionTable table(1);
    Searcher searcher(table);
    SearchLimits limits{.depth = 1};
    limits.maximum_ply = 129;
    const SearchResult result = searcher.search(root, limits);
    CHECK_EQ(result.effective_maximum_ply, 128);
}

TEST_CASE(parallel_root_split_preserves_original_ply_and_mate_distance) {
    Position root = position("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    SearchLimits single_limits{.depth = 4};
    single_limits.maximum_ply = 1;
    single_limits.search_moves = {Move(Square::G6, Square::G7)};
    SearchLimits parallel_limits = single_limits;
    parallel_limits.threads = 2;

    TranspositionTable single_table(1);
    Searcher single_searcher(single_table);
    const SearchResult single = single_searcher.search(root, single_limits);
    TranspositionTable parallel_table(1);
    Searcher parallel_searcher(parallel_table);
    const SearchResult parallel = parallel_searcher.search(root, parallel_limits);

    CHECK_EQ(single.score, search_mate_score - 1);
    CHECK_EQ(parallel.score, single.score);
    CHECK(root.is_legal(parallel.best_move));
}

TEST_CASE(parallel_root_split_preserves_root_check_extension) {
    Position root = position("6k1/8/8/8/8/8/8/3Q2K1 w - - 0 1");
    SearchLimits single_limits{.depth = 4};
    single_limits.maximum_ply = 1;
    single_limits.search_moves = {Move(Square::D1, Square::D8)};
    SearchLimits parallel_limits = single_limits;
    parallel_limits.threads = 2;

    TranspositionTable single_table(1);
    Searcher single_searcher(single_table);
    const SearchResult single = single_searcher.search(root, single_limits);
    TranspositionTable parallel_table(1);
    Searcher parallel_searcher(parallel_table);
    const SearchResult parallel = parallel_searcher.search(root, parallel_limits);

    CHECK_EQ(single.maximum_extension_count, 1);
    CHECK_EQ(parallel.maximum_extension_count, single.maximum_extension_count);
    CHECK_EQ(parallel.score, single.score);
    CHECK(root.is_legal(parallel.best_move));
}

TEST_CASE(parallel_root_move_is_available_to_recapture_extension) {
    Position root = position("4k3/8/4p3/3p4/4P3/8/8/4K3 w - - 0 1");
    SearchLimits single_limits{.depth = 2};
    single_limits.maximum_ply = 2;
    single_limits.search_moves = {Move(Square::E4, Square::D5, MoveFlag::Capture)};
    SearchLimits parallel_limits = single_limits;
    parallel_limits.threads = 2;

    TranspositionTable single_table(1);
    Searcher single_searcher(single_table);
    const SearchResult single = single_searcher.search(root, single_limits);
    TranspositionTable parallel_table(1);
    Searcher parallel_searcher(parallel_table);
    const SearchResult parallel = parallel_searcher.search(root, parallel_limits);

    CHECK(single.maximum_extension_count > 0);
    CHECK_EQ(parallel.maximum_extension_count, single.maximum_extension_count);
    CHECK_EQ(parallel.score, single.score);
    CHECK(root.is_legal(parallel.best_move));
}

TEST_CASE(search_returns_a_legal_move_and_legal_principal_variation) {
    Position root = position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 3});
    CHECK(result.best_move.is_valid());
    CHECK(root.is_legal(result.best_move));
    CHECK_EQ(result.best_move, result.pv.front());
    for (const Move move : result.pv) {
        CHECK(root.is_legal(move));
        StateInfo state;
        CHECK(root.make_move(move, state));
    }
    CHECK(result.depth >= 1);
    CHECK(result.nodes > 0);
}

TEST_CASE(search_honors_node_and_external_stop_limits) {
    Position root = position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    TranspositionTable table(1);
    Searcher searcher(table);
    const SearchResult limited = searcher.search(root, SearchLimits{.depth = 20, .nodes = 200});
    CHECK(limited.best_move.is_valid());
    CHECK(limited.nodes <= 200);

    std::atomic<bool> stopped = true;
    const SearchResult cancelled = searcher.search(root, SearchLimits{.depth = 20}, &stopped);
    CHECK(cancelled.best_move.is_valid());
    CHECK(cancelled.stopped);

    TranspositionTable parallel_table(2);
    Searcher parallel(parallel_table);
    SearchLimits parallel_limits{.depth = 20, .nodes = 200};
    parallel_limits.threads = 4;
    const SearchResult parallel_limited = parallel.search(root, parallel_limits);
    CHECK(parallel_limited.best_move.is_valid());
    CHECK(parallel_limited.nodes <= 200);
}

TEST_CASE(search_finishes_cleanly_between_iterations_at_the_soft_target) {
    Position root = position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    TranspositionTable table(4);
    Searcher searcher(table);
    SearchLimits limits{.depth = 64};
    limits.target_time = std::chrono::milliseconds(1);
    limits.move_time = std::chrono::milliseconds(100);

    const SearchResult result = searcher.search(root, limits);
    CHECK(result.depth >= 1);
    CHECK(result.depth < 64);
    CHECK(!result.stopped);
}

TEST_CASE(search_treats_rule50_position_as_draw) {
    Position root = position("4k3/8/8/8/8/8/3Q4/4K3 b - - 100 1");
    TranspositionTable table(1);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 3});
    CHECK_EQ(result.score, 0);
}

TEST_CASE(search_does_not_reuse_unsafe_rule50_cutoff) {
    TranspositionTable table(4);
    Searcher searcher(table);
    Position low = position("4k3/8/8/8/8/8/3Q4/4K3 b - - 0 1");
    Position near_draw = position("4k3/8/8/8/8/8/3Q4/4K3 b - - 99 1");
    const SearchResult first = searcher.search(low, SearchLimits{.depth = 4});
    const SearchResult second = searcher.search(near_draw, SearchLimits{.depth = 4});
    CHECK(first.score != 0);
    CHECK_EQ(second.score, 0);
}

TEST_CASE(search_does_not_reuse_mismatched_rule50_lower_bound) {
    TranspositionTable table(4);
    Searcher searcher(table);
    Position near_draw = position("4k3/8/8/8/8/8/3Q4/4K3 b - - 99 1");
    table.store(near_draw.key(), Move{}, 120, 8, Bound::Lower, 0, 0);

    const SearchResult result = searcher.debug_search_window(near_draw, 4, -50, 50);

    CHECK_EQ(result.score, 0);
    CHECK(result.nodes > 1);
}

TEST_CASE(search_does_not_reuse_mismatched_rule50_upper_bound) {
    TranspositionTable table(4);
    Searcher searcher(table);
    Position near_draw = position("4k3/8/8/8/8/8/3Q4/4K3 b - - 99 1");
    table.store(near_draw.key(), Move{}, -120, 8, Bound::Upper, 0, 0);

    const SearchResult result = searcher.debug_search_window(near_draw, 4, -50, 50);

    CHECK_EQ(result.score, 0);
    CHECK(result.nodes > 1);
}

TEST_CASE(search_honors_root_searchmoves_restriction) {
    Position root = position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    TranspositionTable table(2);
    Searcher searcher(table);
    SearchLimits limits{.depth = 2};
    limits.search_moves = {Move(Square::E2, Square::E4, MoveFlag::DoublePush)};
    const SearchResult result = searcher.search(root, limits);
    CHECK_EQ(result.best_move, limits.search_moves.front());
    CHECK_EQ(result.pv.front(), limits.search_moves.front());
}

TEST_CASE(search_parallel_root_split_returns_a_legal_result) {
    Position root = position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    TranspositionTable table(8);
    Searcher searcher(table);
    SearchLimits limits{.depth = 3};
    limits.threads = 4;
    const SearchResult result = searcher.search(root, limits);
    CHECK(result.best_move.is_valid());
    CHECK(root.is_legal(result.best_move));
    CHECK(result.depth >= 1);
    CHECK(result.nodes > 0);
    CHECK(!result.stopped);
}

TEST_CASE(parallel_depth_one_searches_moves_instead_of_returning_generation_order) {
    Position root = position("4k3/8/8/8/8/8/q7/R3K3 w - - 0 1");
    TranspositionTable one_table(2);
    TranspositionTable four_table(2);
    Searcher one_searcher(one_table);
    Searcher four_searcher(four_table);
    SearchLimits one{.depth = 1};
    SearchLimits four{.depth = 1};
    four.threads = 4;
    const SearchResult single = one_searcher.search(root, one);
    const SearchResult parallel = four_searcher.search(root, four);
    CHECK_EQ(parallel.best_move, single.best_move);
    CHECK_EQ(parallel.score, single.score);
    CHECK(parallel.nodes > 0);
    CHECK_EQ(move_to_uci(parallel.best_move), "a1a2");
}

TEST_CASE(search_parallel_root_split_preserves_single_thread_score) {
    Position root = position("r1bqk2r/pppp1ppp/2n2n2/4p3/2B1P3/2N2N2/PPPP1PPP/R1BQK2R w KQkq - 4 5");
    TranspositionTable single_table(8);
    Searcher single(single_table);
    const SearchResult one = single.search(root, SearchLimits{.depth = 5});

    TranspositionTable parallel_table(8);
    Searcher parallel(parallel_table);
    SearchLimits limits{.depth = 5};
    limits.threads = 4;
    const SearchResult many = parallel.search(root, limits);

    CHECK_EQ(many.depth, one.depth);
    CHECK_EQ(many.score, one.score);
    CHECK(root.is_legal(many.best_move));
}

TEST_CASE(search_parallel_root_split_preserves_classical_evaluation) {
    Position root = position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    TranspositionTable single_table(8);
    Searcher single(single_table);
    const SearchResult one = single.search(root, SearchLimits{.depth = 2});

    TranspositionTable parallel_table(8);
    Searcher parallel(parallel_table);
    SearchLimits limits{.depth = 2};
    limits.threads = 4;
    const SearchResult many = parallel.search(root, limits);

    CHECK_EQ(one.score, many.score);
}

TEST_CASE(search_start_position_depth_five_stays_under_node_regression_budget) {
    Position root = position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    TranspositionTable table(8);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 5});
    CHECK_EQ(result.depth, 5);
    CHECK(result.nodes < 40000);
}

TEST_CASE(selective_search_keeps_depth_six_under_node_regression_budget) {
    Position root = position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    TranspositionTable table(16);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 6});
    CHECK_EQ(result.depth, 6);
    CHECK(result.nodes < 30000);
}

TEST_CASE(probcut_preserves_scores_on_tactical_legality_corpus) {
    constexpr std::array<std::string_view, 3> fens = {
        "4k3/4q3/8/8/8/8/4R3/4K3 w - - 0 1",
        "4r1k1/8/8/8/8/8/3qR3/4K3 w - - 0 1",
        "r3k2r/ppp2ppp/2n5/3qp3/3P4/2P2N2/PP3PPP/R2Q1RK1 w kq - 0 12",
    };

    for (const std::string_view fen : fens) {
        Position enabled_root = position(fen);
        TranspositionTable enabled_table(4);
        Searcher enabled_searcher(enabled_table);
        SearchLimits enabled_limits{.depth = 5};
        enabled_limits.enable_probcut = true;
        const SearchResult enabled = enabled_searcher.search(enabled_root, enabled_limits);

        Position disabled_root = position(fen);
        TranspositionTable disabled_table(4);
        Searcher disabled_searcher(disabled_table);
        SearchLimits disabled_limits{.depth = 5};
        disabled_limits.enable_probcut = false;
        const SearchResult disabled = disabled_searcher.search(disabled_root, disabled_limits);

        CHECK_EQ(enabled.score, disabled.score);
        if (fen == fens.front()) {
            CHECK(enabled.probcut_legal_checks > 0);
            CHECK_EQ(disabled.probcut_legal_checks, 0U);
        }
        CHECK(enabled_root.is_legal(enabled.best_move));
        CHECK(disabled_root.is_legal(disabled.best_move));
    }
}

TEST_CASE(search_prefers_the_shortest_forced_mate) {
    Position root = position("6k1/8/5K2/8/8/8/8/3Q4 w - - 0 1");
    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 6});
    CHECK(result.score >= search_mate_threshold);
    CHECK_EQ(result.score, search_mate_score - 3);
    CHECK_EQ(move_to_uci(result.best_move), "d1g4");
}

TEST_CASE(mate_distance_bounds_reduce_forced_mate_tree) {
    Position root = position("6k1/8/5K2/8/8/8/8/3Q4 w - - 0 1");
    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 6});
    CHECK(result.nodes < 550);
}

TEST_CASE(null_move_is_used_only_at_non_pv_nodes) {
    Position root = position("r1bqk2r/pppp1ppp/2n2n2/4p3/2B1P3/2N2N2/PPPP1PPP/R1BQK2R w KQkq - 4 5");
    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 8});
    CHECK(result.null_move_searches > 0);
    CHECK_EQ(result.null_move_pv_searches, 0U);
    CHECK(root.is_legal(result.best_move));
}

TEST_CASE(verified_null_move_preserves_zugzwang_score) {
    constexpr std::string_view fen = "8/8/8/2k5/8/2P5/2K5/8 w - - 0 1";
    Position enabled_root = position(fen);
    Position disabled_root = position(fen);
    TranspositionTable enabled_table(4);
    TranspositionTable disabled_table(4);
    Searcher enabled_searcher(enabled_table);
    Searcher disabled_searcher(disabled_table);
    SearchLimits enabled{.depth = 8};
    SearchLimits disabled{.depth = 8};
    disabled.enable_null_move = false;
    const SearchResult with_null = enabled_searcher.search(enabled_root, enabled);
    const SearchResult without_null = disabled_searcher.search(disabled_root, disabled);
    CHECK_EQ(with_null.score, without_null.score);
    CHECK_EQ(with_null.best_move, without_null.best_move);
}

TEST_CASE(high_depth_null_move_verification_matches_disabled_search) {
    constexpr std::string_view fen = "4k3/8/8/8/8/8/R6r/4K3 w - - 0 1";
    Position enabled_root = position(fen);
    Position disabled_root = position(fen);
    TranspositionTable enabled_table(4);
    TranspositionTable disabled_table(4);
    Searcher enabled_searcher(enabled_table);
    Searcher disabled_searcher(disabled_table);
    SearchLimits enabled{.depth = 11};
    SearchLimits disabled{.depth = 11};
    disabled.enable_null_move = false;

    const SearchResult with_null = enabled_searcher.search(enabled_root, enabled);
    const SearchResult without_null = disabled_searcher.search(disabled_root, disabled);

    CHECK(with_null.null_move_searches > 0);
    CHECK(std::abs(with_null.score - without_null.score) < 20);
    CHECK_EQ(with_null.best_move, without_null.best_move);
    CHECK(enabled_root.is_legal(with_null.best_move));
}

TEST_CASE(high_depth_null_move_verification_restores_position_when_stopped) {
    Position root = position("4k3/8/8/8/8/8/R6r/4K3 w - - 0 1");
    TranspositionTable table(4);
    Searcher searcher(table);
    SearchLimits limits{.depth = 12, .nodes = 10'000};

    const SearchResult result = searcher.search(root, limits);

    CHECK(result.stopped);
    CHECK(result.nodes >= limits.nodes);
    CHECK(result.null_move_searches > 0);
    CHECK(root.is_legal(result.best_move));
}

// ===== SEE pruning in quiescence search tests =====

TEST_CASE(see_pruning_retains_winning_capture) {
    // White rook captures undefended black rook on e4 — SEE = +500 (winning).
    constexpr std::string_view fen = "4k3/8/8/8/4r3/8/8/4R1K1 w - - 0 1";
    const auto root = position(fen);
    auto pos = position(fen);
    MoveList legal;
    generate_legal(pos, legal);
    bool found = false;
    for (const Move m : legal) {
        if (move_to_uci(m) == "e1e4") {
            CHECK(see_ge(pos, m, 0));
            found = true;
            break;
        }
    }
    CHECK(found);

    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 3});
    CHECK(result.best_move.is_valid());
}

TEST_CASE(see_pruning_removes_losing_capture) {
    // Black queen on d8 defends rook on d7. White queen on d1 captures d7 — losing (SEE = -500).
    // The losing capture should be pruned by SEE.
    constexpr std::string_view fen = "3q1rk1/3r4/8/8/8/8/8/3QK3 w - - 0 1";
    const auto root = position(fen);
    auto pos = position(fen);
    MoveList legal;
    generate_legal(pos, legal);
    constexpr auto uci = "d1d7";
    bool found_losing = false;
    for (const Move m : legal) {
        if (move_to_uci(m) == uci) {
            CHECK(!see_ge(pos, m, 0));
            found_losing = true;
            break;
        }
    }
    CHECK(found_losing);

    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 3});
    // The losing capture should NOT be the best move (it's a blunder).
    CHECK(move_to_uci(result.best_move) != uci);
    // Instrumentation should show pruning occurred.
    CHECK(result.picker_stats.see_pruning_calls > 0);
    CHECK(result.picker_stats.captures_pruned_by_see > 0);
}

TEST_CASE(see_pruning_retains_equal_exchange) {
    // White rook captures black rook that is defended — equal exchange (SEE = 0).
    // Should be retained at threshold zero.
    constexpr std::string_view fen = "4r1k1/8/8/8/8/8/8/4R1K1 w - - 0 1";
    const auto root = position(fen);
    auto pos = position(fen);
    MoveList legal;
    generate_legal(pos, legal);
    bool found_equal = false;
    for (const Move m : legal) {
        if (move_to_uci(m) == "e1e8") {
            CHECK(see_ge(pos, m, 0));  // SEE >= 0
            found_equal = true;
            break;
        }
    }
    CHECK(found_equal);

    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 3});
    // The equal exchange should still be a candidate.
    CHECK(result.best_move.is_valid());
}

TEST_CASE(see_pruning_retains_tt_capture_even_if_losing) {
    // Position where a losing capture exists. Pre-populate the TT with that losing capture
    // so it becomes the TT move. Despite having SEE < 0, the TT move must not be pruned.
    constexpr std::string_view fen = "3q1rk1/3r4/8/8/8/8/8/3QK3 w - - 0 1";
    auto root = position(fen);

    MoveList legal;
    generate_legal(root, legal);
    Move losing_capture;
    for (const Move m : legal) {
        if (move_to_uci(m) == "d1d7") {
            losing_capture = m;
            break;
        }
    }
    CHECK(losing_capture.is_valid());
    CHECK(!see_ge(root, losing_capture, 0));

    // Pre-populate TT with the losing capture as TT move
    TranspositionTable table(4);
    table.store(root.key(), losing_capture, 0, 3, Bound::Lower, 0, root.rule50(), tt_no_static_evaluation, true);
    Searcher searcher(table);

    const SearchResult result = searcher.search(root, SearchLimits{.depth = 2});
    // The TT move should have been tried and not SEE-pruned.
    // All losing non-TT non-checking captures should be pruned.
    CHECK(result.picker_stats.captures_pruned_by_see > 0);
}

TEST_CASE(see_pruning_retains_checking_capture_even_if_losing) {
    // White rook captures a king-defended pawn on e7 — SEE is negative (rook > pawn).
    // But the capture gives check (rook attacks the king from e7), so it must not be pruned.
    constexpr std::string_view fen = "4k3/4p3/4R3/8/8/8/8/4K3 w - - 0 1";
    auto root = position(fen);
    auto pos = position(fen);
    MoveList legal;
    generate_legal(pos, legal);
    Move checking_capture;
    for (const Move m : legal) {
        if (move_to_uci(m) == "e6e7") {
            checking_capture = m;
            break;
        }
    }
    CHECK(checking_capture.is_valid());
    CHECK(!see_ge(pos, checking_capture, 0));

    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 3});
    CHECK(result.best_move.is_valid());
    CHECK(result.picker_stats.checking_captures_exempted > 0);
}

TEST_CASE(see_pruning_preserves_promotions) {
    // White pawn promotes to queen even though the promotion square is defended.
    // Promotions should never be SEE-pruned.
    constexpr std::string_view fen = "1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1";
    auto root = position(fen);
    MoveList legal;
    generate_legal(root, legal);
    StateInfo st;
    Move promotion;
    for (const Move m : legal) {
        if (move_to_uci(m) == "a7b8q" || move_to_uci(m) == "a7b8r" ||
            move_to_uci(m) == "a7b8b" || move_to_uci(m) == "a7b8n") {
            promotion = m;
            break;
        }
    }
    CHECK(promotion.is_valid());
    // Promotion might have negative SEE (rook defends b8), but should not be pruned

    TranspositionTable table(4);
    Searcher searcher(table);
    // Search with depth=2 (so depth-1 goes to qsearch)
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 2});
    CHECK_EQ(move_to_uci(result.best_move), "a7b8q");
    CHECK(result.picker_stats.promotions_ep_exempted > 0);
}

TEST_CASE(see_pruning_preserves_en_passant) {
    // En-passant should never be SEE-pruned. Verify see_ge handles EP correctly.
    constexpr std::string_view fen = "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1";
    auto pos = position(fen);
    MoveList legal;
    generate_legal(pos, legal);
    Move ep;
    for (const Move m : legal) {
        if (move_to_uci(m) == "e5d6") {
            ep = m;
            break;
        }
    }
    CHECK(ep.is_valid());
    // EP captures the pawn on d5 (en-passant). SEE accounts for the pawn value.
    // The d5 pawn is undefended, so SEE should be +100 (pawn value).
    CHECK(static_exchange_evaluation(pos, ep) > 0);
    CHECK(see_ge(pos, ep, 0));

    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(pos, SearchLimits{.depth = 3});
    CHECK(result.best_move.is_valid());
}

TEST_CASE(see_pruning_in_check_searches_all_evasions) {
    // Black king is in check from white rook on e1 — qsearch must search every evasion.
    constexpr std::string_view fen = "4k3/8/8/8/8/8/8/4R1K1 b - - 0 1";
    auto root = position(fen);
    CHECK(in_check(root));

    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 3});
    CHECK(result.best_move.is_valid());
    // In-check qsearch should SEE-prune nothing (no SEE pruning in check nodes)
    CHECK_EQ(result.picker_stats.captures_pruned_by_see, 0U);
}

TEST_CASE(see_pruning_see_ge_agrees_with_see_exact_on_corpus) {
    // Verify that see_ge(move, 0) == (static_exchange_evaluation(move) >= 0)
    // for every legal capture across a deterministic corpus of positions.
    constexpr std::string_view positions[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "4k3/8/8/3p4/4R3/8/8/4K3 w - - 0 1",
        "4k3/8/8/2b5/3rQ3/8/8/4K3 w - - 0 1",
        "4k3/8/8/8/3pR3/8/8/4K3 w - - 0 1",
        "4k3/8/3p4/8/8/4P3/8/4K3 w - - 0 1",
        "4k3/4n3/8/5p2/4B3/8/8/4R1K1 w - - 0 1",
        "1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1",
        "1R2k3/Pr6/8/8/8/8/8/4K3 b - - 0 1",
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
        "4r1k1/8/8/8/8/8/8/4R1K1 w - - 0 1",
        "3q1rk1/3r4/8/8/8/8/8/3QK3 w - - 0 1",
        "4r1k1/8/8/8/8/8/8/Q3K3 w - - 0 1",
        "4k3/8/8/8/8/8/R6r/4K3 w - - 0 1",
        "rnbqkb1r/pppppppp/5n2/4P3/8/8/PPPP1PPP/RNBQKBNR w KQkq - 0 1",
        "1k1r4/pp1b1pp1/4p2p/2pn4/2nN4/2P1BN2/PP2QPPP/R4RK1 w - - 0 1",
        "r3k2r/pbp2ppp/1pnp4/4N3/1PP5/2N1q3/P4PPP/R2R2K1 w kq - 0 1",
    };

    for (const auto& fen : positions) {
        const auto maybe_pos = Position::from_fen(fen);
        if (!maybe_pos) continue;
        Position pos = *maybe_pos;
        MoveList all_moves;
        generate_pseudo_legal(pos, all_moves, GenType::Captures);
        for (std::size_t i = 0; i < all_moves.size(); ++i) {
            const Move m = all_moves[i];
            if (!m.has_flag(MoveFlag::Capture) && !m.has_flag(MoveFlag::EnPassant)) continue;
            const int see_value = static_exchange_evaluation(pos, m);
            const bool see_ge_result = see_ge(pos, m, 0);
            if ((see_value >= 0) != see_ge_result) {
                std::cerr << "Corpus mismatch in position: " << fen
                          << "\n  move=" << move_to_uci(m)
                          << " see=" << see_value
                          << " see_ge(0)=" << see_ge_result << "\n";
            }
            CHECK_EQ(see_ge_result, see_value >= 0);
        }
    }
}

}  // namespace
}  // namespace blaze
