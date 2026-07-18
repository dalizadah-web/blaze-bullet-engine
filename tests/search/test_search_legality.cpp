#include "blaze/search/search_legality.h"
#include "test_support.h"

namespace blaze {
namespace {

Position parsed_position(std::string_view fen) {
    const auto parsed = Position::from_fen(fen);
    CHECK(parsed.has_value());
    return *parsed;
}

TEST_CASE(last_move_legality_checks_the_player_who_just_moved) {
    Position checking_capture = parsed_position(
        "4k3/4q3/8/8/8/8/4R3/4K3 w - - 0 1");
    StateInfo capture_state;
    CHECK(checking_capture.make_move(
        Move(Square::E2, Square::E7, MoveFlag::Capture), capture_state));
    CHECK(last_move_kept_own_king_safe(checking_capture));

    Position self_check = parsed_position(
        "k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1");
    StateInfo quiet_state;
    CHECK(self_check.make_move(Move(Square::E2, Square::A2), quiet_state));
    CHECK(!last_move_kept_own_king_safe(self_check));
}

}  // namespace
}  // namespace blaze
