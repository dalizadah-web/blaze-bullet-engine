#include "blaze/search/search.h"

#include "blaze/core/movegen.h"
#include "test_support.h"

#include <atomic>
#include <chrono>
#include <string_view>

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
