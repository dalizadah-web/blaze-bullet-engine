#include "test_support.h"

#include "blaze/core/position.h"

#include <bit>
#include <string_view>

namespace {

constexpr std::string_view start_fen =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

}  // namespace

TEST_CASE(start_position_fen_round_trips_exactly) {
    const auto position = blaze::Position::from_fen(start_fen);
    CHECK(position.has_value());
    CHECK(position->is_consistent());
    CHECK_EQ(position->to_fen(), start_fen);
}

TEST_CASE(start_position_exposes_exact_piece_state) {
    const auto position = blaze::Position::from_fen(start_fen);
    CHECK(position.has_value());

    CHECK_EQ(std::popcount(position->occupied()), 32);
    CHECK_EQ(
        std::popcount(position->pieces(blaze::Color::White, blaze::PieceType::Pawn)),
        8);
    CHECK_EQ(
        std::popcount(position->pieces(blaze::Color::Black, blaze::PieceType::Pawn)),
        8);
    CHECK_EQ(position->piece_on(blaze::Square::E1), blaze::Piece::WhiteKing);
    CHECK_EQ(position->piece_on(blaze::Square::E8), blaze::Piece::BlackKing);
    CHECK_EQ(position->piece_on(blaze::Square::E4), blaze::Piece::None);
    CHECK_EQ(position->side_to_move(), blaze::Color::White);
    CHECK_EQ(
        position->castling_rights(),
        blaze::castling_mask(
            blaze::CastlingRight::WhiteKing,
            blaze::CastlingRight::WhiteQueen,
            blaze::CastlingRight::BlackKing,
            blaze::CastlingRight::BlackQueen));
    CHECK_EQ(position->ep_square(), blaze::Square::None);
    CHECK_EQ(position->rule50(), 0);
    CHECK_EQ(position->fullmove_number(), 1);
}

TEST_CASE(fen_preserves_side_and_clocks) {
    constexpr std::string_view fen = "4k3/8/8/8/8/8/8/4K3 b - - 37 88";
    const auto position = blaze::Position::from_fen(fen);
    CHECK(position.has_value());
    CHECK_EQ(position->side_to_move(), blaze::Color::Black);
    CHECK_EQ(position->rule50(), 37);
    CHECK_EQ(position->fullmove_number(), 88);
    CHECK_EQ(position->to_fen(), fen);
}

TEST_CASE(fen_accepts_valid_en_passant_targets) {
    const auto black_to_move = blaze::Position::from_fen(
        "4k3/8/8/8/4P3/8/8/4K3 b - e3 0 1");
    const auto white_to_move = blaze::Position::from_fen(
        "4k3/8/8/4p3/8/8/8/4K3 w - e6 0 2");

    CHECK(black_to_move.has_value());
    CHECK_EQ(black_to_move->ep_square(), blaze::Square::E3);
    CHECK(white_to_move.has_value());
    CHECK_EQ(white_to_move->ep_square(), blaze::Square::E6);
}

TEST_CASE(fen_rejects_missing_or_adjacent_kings) {
    CHECK(!blaze::Position::from_fen("8/8/8/8/8/8/8/8 w - - 0 1").has_value());
    CHECK(!blaze::Position::from_fen("4k3/8/8/8/8/8/8/8 w - - 0 1").has_value());
    CHECK(!blaze::Position::from_fen("8/8/8/8/8/8/4k3/4K3 w - - 0 1").has_value());
}

TEST_CASE(fen_rejects_malformed_board_and_fields) {
    CHECK(!blaze::Position::from_fen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1")
               .has_value());
    CHECK(!blaze::Position::from_fen("4k3/8/8/8/8/8/8/9K w - - 0 1").has_value());
    CHECK(!blaze::Position::from_fen("4k3/8/8/8/8/8/8/4K3 w - e4 0 1").has_value());
    CHECK(!blaze::Position::from_fen("4k3/8/8/8/8/8/8/4K3 w - - -1 1").has_value());
    CHECK(!blaze::Position::from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 0").has_value());
    CHECK(!blaze::Position::from_fen("4k3/8/8/8/8/8/8/P3K3 w - - 0 1").has_value());
}

TEST_CASE(fen_rejects_castling_rights_without_home_pieces) {
    CHECK(!blaze::Position::from_fen("4k3/8/8/8/8/8/8/4K3 w K - 0 1").has_value());
    CHECK(!blaze::Position::from_fen("4k3/8/8/8/8/8/8/4K3 w Q - 0 1").has_value());
    CHECK(!blaze::Position::from_fen("4k3/8/8/8/8/8/8/4K3 w k - 0 1").has_value());
    CHECK(!blaze::Position::from_fen("4k3/8/8/8/8/8/8/4K3 w q - 0 1").has_value());
}
