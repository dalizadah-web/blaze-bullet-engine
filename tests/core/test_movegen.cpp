#include "test_support.h"

#include "blaze/core/movegen.h"

#include <string_view>

namespace {

blaze::Position position_from(std::string_view fen) {
    auto position = blaze::Position::from_fen(fen);
    CHECK(position.has_value());
    return *position;
}

bool contains_uci(const blaze::MoveList& moves, std::string_view uci) {
    for (const blaze::Move move : moves) {
        if (blaze::move_to_uci(move) == uci) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE(start_position_has_twenty_legal_moves) {
    auto position = position_from(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    blaze::MoveList moves;
    blaze::generate_legal(position, moves);

    CHECK_EQ(moves.size(), 20U);
    CHECK(contains_uci(moves, "e2e3"));
    CHECK(contains_uci(moves, "e2e4"));
    CHECK(contains_uci(moves, "g1f3"));
}

TEST_CASE(pinned_piece_cannot_leave_king_exposed) {
    auto position = position_from("k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1");
    blaze::MoveList moves;
    blaze::generate_legal(position, moves);

    CHECK(!contains_uci(moves, "e2d2"));
    CHECK(contains_uci(moves, "e2e8"));
}

TEST_CASE(double_check_allows_only_king_moves) {
    auto position = position_from("k3r3/8/8/8/1b6/8/8/4K1N1 w - - 0 1");
    blaze::MoveList moves;
    blaze::generate_legal(position, moves);

    CHECK(blaze::in_check(position));
    for (const blaze::Move move : moves) {
        CHECK_EQ(move.from(), blaze::Square::E1);
    }
}

TEST_CASE(en_passant_cannot_expose_own_king) {
    auto position = position_from("k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
    blaze::MoveList moves;
    blaze::generate_legal(position, moves);

    CHECK(!contains_uci(moves, "e5d6"));
}

TEST_CASE(castling_through_attacked_square_is_illegal) {
    auto position = position_from("k4r2/8/8/8/8/8/8/4K2R w K - 0 1");
    blaze::MoveList moves;
    blaze::generate_legal(position, moves);

    CHECK(!contains_uci(moves, "e1g1"));
}

TEST_CASE(castling_is_generated_when_path_is_clear_and_safe) {
    auto position = position_from("4k3/8/8/8/8/8/8/4K2R w K - 0 1");
    blaze::MoveList moves;
    blaze::generate_legal(position, moves);

    CHECK(contains_uci(moves, "e1g1"));
}

TEST_CASE(all_four_quiet_promotions_are_generated) {
    auto position = position_from("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    blaze::MoveList moves;
    blaze::generate_legal(position, moves);

    CHECK(contains_uci(moves, "a7a8q"));
    CHECK(contains_uci(moves, "a7a8r"));
    CHECK(contains_uci(moves, "a7a8b"));
    CHECK(contains_uci(moves, "a7a8n"));
}

TEST_CASE(checkmate_has_no_legal_moves_and_is_check) {
    auto position = position_from("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1");
    blaze::MoveList moves;
    blaze::generate_legal(position, moves);

    CHECK(blaze::in_check(position));
    CHECK_EQ(moves.size(), 0U);
}

TEST_CASE(stalemate_has_no_legal_moves_and_is_not_check) {
    auto position = position_from("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    blaze::MoveList moves;
    blaze::generate_legal(position, moves);

    CHECK(!blaze::in_check(position));
    CHECK_EQ(moves.size(), 0U);
}
