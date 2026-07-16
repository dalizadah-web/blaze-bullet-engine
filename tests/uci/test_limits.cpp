#include "blaze/uci/limits.h"

#include "test_support.h"

#include <chrono>
#include <string>

namespace blaze {
namespace {

TEST_CASE(go_parser_accepts_standard_limits) {
    std::string error;
    const auto go = parse_go("wtime 60000 btime 50000 winc 1000 binc 500 depth 12 nodes 9000", error);
    CHECK(go.has_value());
    CHECK_EQ(go->white_time.count(), 60000);
    CHECK_EQ(go->black_time.count(), 50000);
    CHECK_EQ(go->depth, 12);
    CHECK_EQ(go->nodes, 9000U);
}

TEST_CASE(go_parser_rejects_missing_negative_and_unknown_values) {
    std::string error;
    CHECK(!parse_go("depth", error).has_value());
    CHECK(!parse_go("movetime -1", error).has_value());
    CHECK(!parse_go("mystery 5", error).has_value());
}

TEST_CASE(movetime_is_an_explicit_search_budget) {
    std::string error;
    const auto go = parse_go("movetime 250 depth 30", error);
    CHECK(go.has_value());
    const SearchLimits limits = to_search_limits(*go, Color::White);
    CHECK_EQ(limits.move_time.count(), 250);
    CHECK_EQ(limits.depth, 30);
}

TEST_CASE(clock_budget_uses_side_to_move_and_never_consumes_entire_clock) {
    std::string error;
    const auto go = parse_go("wtime 10000 btime 2000 winc 1000 binc 0 movestogo 20", error);
    CHECK(go.has_value());
    const SearchLimits white = to_search_limits(*go, Color::White);
    const SearchLimits black = to_search_limits(*go, Color::Black);
    CHECK(white.move_time > black.move_time);
    CHECK(white.move_time.count() > 0);
    CHECK(white.move_time.count() < 10000);
    CHECK(black.move_time.count() < 2000);
}

TEST_CASE(infinite_and_ponder_searches_have_no_clock_deadline) {
    std::string error;
    const auto infinite = parse_go("infinite", error);
    const auto ponder = parse_go("ponder wtime 1000 btime 1000", error);
    CHECK(infinite.has_value());
    CHECK(ponder.has_value());
    CHECK_EQ(to_search_limits(*infinite, Color::White).move_time.count(), 0);
    CHECK_EQ(to_search_limits(*ponder, Color::White).move_time.count(), 0);
}

}  // namespace
}  // namespace blaze
