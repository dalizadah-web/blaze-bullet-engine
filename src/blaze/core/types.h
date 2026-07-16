#ifndef BLAZE_CORE_TYPES_H
#define BLAZE_CORE_TYPES_H

#include <cstdint>

namespace blaze {

using Bitboard = std::uint64_t;

enum class Color : std::uint8_t {
    White = 0,
    Black = 1,
};

enum class PieceType : std::uint8_t {
    None = 0,
    Pawn = 1,
    Knight = 2,
    Bishop = 3,
    Rook = 4,
    Queen = 5,
    King = 6,
};

enum class Piece : std::uint8_t {
    None = 0,
    WhitePawn,
    WhiteKnight,
    WhiteBishop,
    WhiteRook,
    WhiteQueen,
    WhiteKing,
    BlackPawn,
    BlackKnight,
    BlackBishop,
    BlackRook,
    BlackQueen,
    BlackKing,
};

[[nodiscard]] constexpr Piece make_piece(Color color, PieceType type) {
    if (type == PieceType::None) {
        return Piece::None;
    }
    const unsigned color_offset = color == Color::White ? 0U : 6U;
    return static_cast<Piece>(color_offset + static_cast<unsigned>(type));
}

[[nodiscard]] constexpr int piece_index(Piece piece) {
    return piece == Piece::None ? -1 : static_cast<int>(piece) - 1;
}

enum class Square : std::uint8_t {
    A1 = 0, B1, C1, D1, E1, F1, G1, H1,
    A2, B2, C2, D2, E2, F2, G2, H2,
    A3, B3, C3, D3, E3, F3, G3, H3,
    A4, B4, C4, D4, E4, F4, G4, H4,
    A5, B5, C5, D5, E5, F5, G5, H5,
    A6, B6, C6, D6, E6, F6, G6, H6,
    A7, B7, C7, D7, E7, F7, G7, H7,
    A8, B8, C8, D8, E8, F8, G8, H8,
    None = 64,
};

[[nodiscard]] constexpr int square_index(Square square) {
    return static_cast<int>(square);
}

[[nodiscard]] constexpr bool is_valid_square(Square square) {
    return square_index(square) >= 0 && square_index(square) < 64;
}

[[nodiscard]] constexpr int file_of(Square square) {
    return square_index(square) & 7;
}

[[nodiscard]] constexpr int rank_of(Square square) {
    return square_index(square) >> 3;
}

[[nodiscard]] constexpr Square make_square(int file, int rank) {
    return file >= 0 && file < 8 && rank >= 0 && rank < 8
        ? static_cast<Square>(rank * 8 + file)
        : Square::None;
}

[[nodiscard]] constexpr Bitboard square_bit(Square square) {
    return is_valid_square(square)
        ? (Bitboard{1} << static_cast<unsigned>(square_index(square)))
        : Bitboard{0};
}

}  // namespace blaze

#endif  // BLAZE_CORE_TYPES_H
