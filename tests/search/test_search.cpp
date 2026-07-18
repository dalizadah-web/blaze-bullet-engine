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

TEST_CASE(search_parallel_root_split_preserves_loaded_network_evaluation) {
    constexpr std::size_t input_bytes = 768U * 256U * 2U;
    constexpr std::size_t hidden_bias_bytes = 256U * 4U;
    constexpr std::size_t output_weight_bytes = 256U * 2U;
    constexpr int network_score = 1200;
    std::vector<std::uint8_t> payload(
        input_bytes + hidden_bias_bytes + output_weight_bytes + 4U,
        0);
    const std::uint32_t raw_bias = static_cast<std::uint32_t>(network_score * 4096);
    const std::size_t bias_offset = payload.size() - 4U;
    for (unsigned byte = 0; byte < 4; ++byte) {
        payload[bias_offset + byte] = static_cast<std::uint8_t>(raw_bias >> (byte * 8U));
    }

    Network network;
    network.version = 1;
    network.features = 768;
    network.hidden = 256;
    network.weights = std::move(payload);
    std::string error;
    const auto evaluator = NetworkEvaluator::create(network, error);
    CHECK(evaluator.has_value());

    Position root = position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    TranspositionTable single_table(8);
    Searcher single(single_table, &*evaluator);
    const SearchResult one = single.search(root, SearchLimits{.depth = 2});

    TranspositionTable parallel_table(8);
    Searcher parallel(parallel_table, &*evaluator);
    SearchLimits limits{.depth = 2};
    limits.threads = 4;
    const SearchResult many = parallel.search(root, limits);

    CHECK_EQ(one.score, network_score);
    CHECK_EQ(many.score, one.score);
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

    CHECK(with_null.null_move_verifications > 0);
    CHECK_EQ(with_null.score, without_null.score);
    CHECK_EQ(with_null.best_move, without_null.best_move);
    CHECK(enabled_root.is_legal(with_null.best_move));
}

TEST_CASE(high_depth_null_move_verification_restores_position_when_stopped) {
    Position root = position("4k3/8/8/8/8/8/R6r/4K3 w - - 0 1");
    TranspositionTable table(4);
    Searcher searcher(table);
    SearchLimits limits{.depth = 12, .nodes = 105'000};

    const SearchResult result = searcher.search(root, limits);

    CHECK(result.stopped);
    CHECK_EQ(result.nodes, limits.nodes);
    CHECK(result.null_move_verifications >= 1U);
    CHECK(root.is_legal(result.best_move));
}

}  // namespace
}  // namespace blaze
