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
    CHECK(result.nodes < 390);
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

}  // namespace
}  // namespace blaze
