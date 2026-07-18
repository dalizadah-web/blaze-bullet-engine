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
    CHECK(!parse_go("winc -1", error).has_value());
    CHECK(!parse_go("binc -1", error).has_value());
    CHECK(!parse_go("mystery 5", error).has_value());
}

TEST_CASE(go_parser_clamps_negative_gui_clock_values_to_zero) {
    std::string error;
    const auto go = parse_go("wtime -22 btime -7 winc 10 binc 10", error);
    CHECK(go.has_value());
    if (!go) return;
    CHECK_EQ(go->white_time.count(), 0);
    CHECK_EQ(go->black_time.count(), 0);
    CHECK(go->white_time_supplied);
    CHECK(go->black_time_supplied);
}

TEST_CASE(supplied_expired_clock_receives_a_finite_emergency_deadline) {
    std::string error;
    const auto go = parse_go("wtime 0 btime 0", error);
    CHECK(go.has_value());
    if (!go) return;

    const SearchLimits limits = to_search_limits(*go, Color::White);
    CHECK_EQ(limits.target_time.count(), 1);
    CHECK_EQ(limits.move_time.count(), 1);
    CHECK_EQ(limits.regime, SearchRegime::Emergency);
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

TEST_CASE(pure_increment_go_command_receives_a_search_deadline) {
    std::string error;
    const auto one_second = parse_go("wtime 0 btime 0 winc 1000 binc 1000", error);
    const auto two_seconds = parse_go("wtime 0 btime 0 winc 2000 binc 2000", error);
    CHECK(one_second.has_value());
    CHECK(two_seconds.has_value());

    const SearchLimits one_second_limits = to_search_limits(*one_second, Color::White);
    const SearchLimits two_second_limits = to_search_limits(*two_seconds, Color::White);
    CHECK(one_second_limits.move_time.count() > 0);
    CHECK(one_second_limits.move_time.count() < 1000);
    CHECK(two_second_limits.move_time > one_second_limits.move_time);
}

TEST_CASE(uci_clock_budget_uses_configured_and_measured_latency) {
    std::string error;
    const auto go = parse_go("wtime 1000 btime 1000", error);
    CHECK(go.has_value());
    const SearchLimits limits = to_search_limits(
        *go, Color::White,
        LatencyBudget{std::chrono::milliseconds(25), std::chrono::milliseconds(80)});
    CHECK(limits.move_time <= std::chrono::milliseconds(920));
    CHECK(limits.target_time < limits.move_time);
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

TEST_CASE(go_parser_preserves_searchmoves_and_mate_requests) {
    std::string error;
    const auto go = parse_go("searchmoves e2e4 d2d4 mate 3", error);
    CHECK(go.has_value());
    CHECK_EQ(go->search_moves.size(), 2U);
    CHECK_EQ(go->search_moves[0], "e2e4");
    CHECK_EQ(go->search_moves[1], "d2d4");
    CHECK_EQ(go->mate, 3);
    const SearchLimits limits = to_search_limits(*go, Color::White);
    CHECK_EQ(limits.mate, 3);
    CHECK_EQ(limits.depth, 6);
}

}  // namespace
}  // namespace blaze
