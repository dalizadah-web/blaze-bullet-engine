#include "blaze/search/search.h"

#include "blaze/core/movegen.h"
#include "test_support.h"

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

TEST_CASE(search_parallel_root_split_returns_a_complete_legal_adaptive_search) {
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
    CHECK(root.is_legal(many.best_move));
    CHECK(!many.stopped);
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

}  // namespace
}  // namespace blaze
