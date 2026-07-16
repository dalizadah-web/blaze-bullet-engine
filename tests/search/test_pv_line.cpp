#include "blaze/search/pv_line.h"

#include "test_support.h"

namespace blaze {
namespace {

TEST_CASE(fixed_capacity_pv_line_prepends_child_without_heap_growth) {
    const Move move_a(Square::E2, Square::E4, MoveFlag::DoublePush);
    const Move move_b(Square::E7, Square::E5, MoveFlag::DoublePush);

    PvLine child;
    child.prepend(move_b, PvLine{});
    PvLine parent;
    parent.prepend(move_a, child);

    CHECK_EQ(parent.size(), 2U);
    CHECK_EQ(parent[0], move_a);
    CHECK_EQ(parent[1], move_b);
}

TEST_CASE(fixed_capacity_pv_line_truncates_at_search_ply_limit) {
    PvLine line;
    for (std::size_t index = 0; index < PvLine::capacity + 20U; ++index) {
        PvLine previous = line;
        line.prepend(Move(Square::A2, Square::A3), previous);
    }
    CHECK_EQ(line.size(), PvLine::capacity);
}

}  // namespace
}  // namespace blaze
