#include "blaze/eval/classical.h"

#include "blaze/core/attacks.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>

namespace blaze {
namespace {

constexpr std::array<int, 7> material = {0, 100, 320, 335, 500, 900, 0};

int relative_rank(Color color, Square square) {
    return color == Color::White ? rank_of(square) : 7 - rank_of(square);
}

int center_distance(Square square) {
    const int file_distance = std::min(std::abs(file_of(square) - 3), std::abs(file_of(square) - 4));
    const int rank_distance = std::min(std::abs(rank_of(square) - 3), std::abs(rank_of(square) - 4));
    return file_distance + rank_distance;
}

int positional_value(PieceType type, Color color, Square square) {
    const int rank = relative_rank(color, square);
    const int center = center_distance(square);
    switch (type) {
        case PieceType::Pawn:
            return rank * 8 - center * 2;
        case PieceType::Knight:
            return 32 - center * 10;
        case PieceType::Bishop:
            return 22 - center * 5;
        case PieceType::Rook:
            return rank * 3 - center;
        case PieceType::Queen:
            return 10 - center * 2;
        case PieceType::King:
            return rank <= 1 ? -center * 3 : -rank * 8 - center * 2;
        case PieceType::None:
            return 0;
    }
    return 0;
}

Bitboard color_occupancy(const Position& position, Color color) {
    Bitboard result = 0;
    for (int type = static_cast<int>(PieceType::Pawn);
         type <= static_cast<int>(PieceType::King);
         ++type) {
        result |= position.pieces(color, static_cast<PieceType>(type));
    }
    return result;
}

int mobility(const Position& position, Color color, Bitboard own) {
    int score = 0;
    for (int index = 0; index < 64; ++index) {
        const Square square = static_cast<Square>(index);
        const Piece piece = position.piece_on(square);
        if (piece == Piece::None || color_of(piece) != color) {
            continue;
        }

        Bitboard attacks = 0;
        switch (type_of(piece)) {
            case PieceType::Knight: attacks = Attacks::knight(square); break;
            case PieceType::Bishop: attacks = Attacks::bishop(square, position.occupied()); break;
            case PieceType::Rook: attacks = Attacks::rook(square, position.occupied()); break;
            case PieceType::Queen: attacks = Attacks::queen(square, position.occupied()); break;
            default: break;
        }
        score += std::popcount(attacks & ~own);
    }
    return score * 2;
}

int pawn_structure(const Position& position, Color color) {
    const Bitboard pawns = position.pieces(color, PieceType::Pawn);
    int score = 0;
    for (int file = 0; file < 8; ++file) {
        int count = 0;
        for (int rank = 0; rank < 8; ++rank) {
            count += (pawns & square_bit(make_square(file, rank))) != 0 ? 1 : 0;
        }
        if (count > 1) {
            score -= (count - 1) * 12;
        }
        if (count > 0) {
            const bool left = file > 0 && (pawns & (Bitboard{0x0101010101010101} << (file - 1))) != 0;
            const bool right = file < 7 && (pawns & (Bitboard{0x0101010101010101} << (file + 1))) != 0;
            if (!left && !right) {
                score -= count * 10;
            }
        }
    }
    return score;
}

int score_color(const Position& position, Color color) {
    int score = 0;
    for (int index = 0; index < 64; ++index) {
        const Square square = static_cast<Square>(index);
        const Piece piece = position.piece_on(square);
        if (piece == Piece::None || color_of(piece) != color) {
            continue;
        }
        const PieceType type = type_of(piece);
        score += material[static_cast<std::size_t>(type)];
        score += positional_value(type, color, square);
    }
    const Bitboard own = color_occupancy(position, color);
    score += mobility(position, color, own);
    score += pawn_structure(position, color);
    return score;
}

}  // namespace

int evaluate(const Position& position) {
    Attacks::initialize();
    const int white_score = score_color(position, Color::White);
    const int black_score = score_color(position, Color::Black);
    const int relative = position.side_to_move() == Color::White
        ? white_score - black_score
        : black_score - white_score;
    return std::clamp(relative, -search_mate_threshold + 1, search_mate_threshold - 1);
}

}  // namespace blaze
