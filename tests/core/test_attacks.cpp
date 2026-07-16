#include "test_support.h"

#include "blaze/core/attacks.h"

#include <initializer_list>

namespace {

blaze::Bitboard bits(std::initializer_list<blaze::Square> squares) {
    blaze::Bitboard result = 0;
    for (const blaze::Square square : squares) {
        result |= blaze::Bitboard{1} << static_cast<unsigned>(square);
    }
    return result;
}

}  // namespace

TEST_CASE(pawn_attacks_follow_color_direction) {
    blaze::Attacks::initialize();
    CHECK_EQ(
        blaze::Attacks::pawn(blaze::Color::White, blaze::Square::A1),
        bits({blaze::Square::B2}));
    CHECK_EQ(
        blaze::Attacks::pawn(blaze::Color::White, blaze::Square::D4),
        bits({blaze::Square::C5, blaze::Square::E5}));
    CHECK_EQ(
        blaze::Attacks::pawn(blaze::Color::Black, blaze::Square::D4),
        bits({blaze::Square::C3, blaze::Square::E3}));
    CHECK_EQ(
        blaze::Attacks::pawn(blaze::Color::Black, blaze::Square::H8),
        bits({blaze::Square::G7}));
}

TEST_CASE(knight_attacks_do_not_wrap_board_edges) {
    blaze::Attacks::initialize();
    CHECK_EQ(
        blaze::Attacks::knight(blaze::Square::A1),
        bits({blaze::Square::B3, blaze::Square::C2}));
    CHECK_EQ(
        blaze::Attacks::knight(blaze::Square::D4),
        bits({
            blaze::Square::B3,
            blaze::Square::B5,
            blaze::Square::C2,
            blaze::Square::C6,
            blaze::Square::E2,
            blaze::Square::E6,
            blaze::Square::F3,
            blaze::Square::F5}));
    CHECK_EQ(
        blaze::Attacks::knight(blaze::Square::H8),
        bits({blaze::Square::F7, blaze::Square::G6}));
}

TEST_CASE(king_attacks_do_not_wrap_board_edges) {
    blaze::Attacks::initialize();
    CHECK_EQ(
        blaze::Attacks::king(blaze::Square::A1),
        bits({blaze::Square::A2, blaze::Square::B1, blaze::Square::B2}));
    CHECK_EQ(
        blaze::Attacks::king(blaze::Square::D4),
        bits({
            blaze::Square::C3,
            blaze::Square::D3,
            blaze::Square::E3,
            blaze::Square::C4,
            blaze::Square::E4,
            blaze::Square::C5,
            blaze::Square::D5,
            blaze::Square::E5}));
}

TEST_CASE(rook_rays_include_first_blocker_and_stop) {
    blaze::Attacks::initialize();
    const blaze::Bitboard occupied = bits({
        blaze::Square::D6,
        blaze::Square::D2,
        blaze::Square::B4,
        blaze::Square::F4});
    const blaze::Bitboard expected = bits({
        blaze::Square::D5,
        blaze::Square::D6,
        blaze::Square::D3,
        blaze::Square::D2,
        blaze::Square::C4,
        blaze::Square::B4,
        blaze::Square::E4,
        blaze::Square::F4});

    CHECK_EQ(blaze::Attacks::rook(blaze::Square::D4, occupied), expected);
}

TEST_CASE(bishop_rays_include_first_blocker_and_stop) {
    blaze::Attacks::initialize();
    const blaze::Bitboard occupied = bits({
        blaze::Square::F6,
        blaze::Square::B6,
        blaze::Square::F2,
        blaze::Square::B2});
    const blaze::Bitboard expected = bits({
        blaze::Square::E5,
        blaze::Square::F6,
        blaze::Square::C5,
        blaze::Square::B6,
        blaze::Square::E3,
        blaze::Square::F2,
        blaze::Square::C3,
        blaze::Square::B2});

    CHECK_EQ(blaze::Attacks::bishop(blaze::Square::D4, occupied), expected);
}

TEST_CASE(queen_attacks_are_bishop_and_rook_union) {
    blaze::Attacks::initialize();
    const blaze::Bitboard occupied = bits({blaze::Square::D6, blaze::Square::F6});
    CHECK_EQ(
        blaze::Attacks::queen(blaze::Square::D4, occupied),
        blaze::Attacks::rook(blaze::Square::D4, occupied) |
            blaze::Attacks::bishop(blaze::Square::D4, occupied));
}
