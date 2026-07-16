#include "blaze/eval/classical.h"

#include "blaze/core/position.h"
#include "test_support.h"

#include <string_view>

namespace blaze {
namespace {

Position position(std::string_view fen) {
    const auto parsed = Position::from_fen(fen);
    CHECK(parsed.has_value());
    return *parsed;
}

TEST_CASE(start_position_evaluates_as_equal) {
    Position start = position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK_EQ(evaluate(start), 0);
}

TEST_CASE(evaluation_is_relative_to_side_to_move) {
    Position white = position("4k3/8/8/8/8/8/3Q4/4K3 w - - 0 1");
    Position black = position("4k3/8/8/8/8/8/3Q4/4K3 b - - 0 1");
    CHECK(evaluate(white) > 800);
    CHECK_EQ(evaluate(white), -evaluate(black));
}

TEST_CASE(material_values_order_major_and_minor_pieces) {
    Position queen = position("4k3/8/8/8/8/8/3Q4/4K3 w - - 0 1");
    Position rook = position("4k3/8/8/8/8/8/3R4/4K3 w - - 0 1");
    Position bishop = position("4k3/8/8/8/8/8/3B4/4K3 w - - 0 1");
    CHECK(evaluate(queen) > evaluate(rook));
    CHECK(evaluate(rook) > evaluate(bishop));
}

TEST_CASE(central_knight_is_preferred_to_corner_knight) {
    Position center = position("4k3/8/8/8/3N4/8/8/4K3 w - - 0 1");
    Position corner = position("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    CHECK(evaluate(center) > evaluate(corner));
}

TEST_CASE(static_evaluation_stays_outside_mate_score_band) {
    Position winning = position("4k3/QQQQQQQQ/8/8/8/8/8/4K3 w - - 0 1");
    const int score = evaluate(winning);
    CHECK(score > 0);
    CHECK(score < search_mate_threshold);
}

}  // namespace
}  // namespace blaze
