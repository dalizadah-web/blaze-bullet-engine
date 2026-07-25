#include "blaze/search/see.h"

#include "blaze/core/movegen.h"
#include "test_support.h"

#include <string_view>

namespace blaze {
namespace {

Position position(std::string_view fen) {
    const auto parsed = Position::from_fen(fen);
    CHECK(parsed.has_value());
    return *parsed;
}

Move find_move(Position& root, std::string_view uci) {
    MoveList legal;
    generate_legal(root, legal);
    for (const Move move : legal) {
        if (move_to_uci(move) == uci) return move;
    }
    return {};
}

TEST_CASE(see_values_winning_and_losing_exchanges) {
    Position winning = position("4k3/8/8/8/3pR3/8/8/4K3 w - - 0 1");
    CHECK(static_exchange_evaluation(winning, find_move(winning, "e4d4")) > 0);

    Position losing = position("4k3/8/8/2b5/3rQ3/8/8/4K3 w - - 0 1");
    CHECK_EQ(static_exchange_evaluation(losing, find_move(losing, "e4d4")), -400);
}

TEST_CASE(see_accounts_for_en_passant_and_promotion) {
    Position ep = position("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
    CHECK(static_exchange_evaluation(ep, find_move(ep, "e5d6")) > 0);

    Position promotion = position("1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    CHECK(static_exchange_evaluation(promotion, find_move(promotion, "a7b8q")) > 0);
}

TEST_CASE(see_accounts_for_promoting_recaptures) {
    Position root = position("1R2k3/Pr6/8/8/8/8/8/4K3 b - - 0 1");
    const Move capture = find_move(root, "b7b8");
    CHECK(capture.is_valid());
    CHECK_EQ(static_exchange_evaluation(root, capture), -800);
}

TEST_CASE(see_excludes_pinned_recaptures) {
    Position root = position("4k3/4n3/8/5p2/4B3/8/8/4R1K1 w - - 0 1");
    const Move capture = find_move(root, "e4f5");
    CHECK(capture.is_valid());
    CHECK(static_exchange_evaluation(root, capture) >= 100);
}

TEST_CASE(see_handles_en_passant_discovered_lines) {
    Position root = position("4r1k1/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
    CHECK(!find_move(root, "e5d6").is_valid());
}

}  // namespace
}  // namespace blaze
