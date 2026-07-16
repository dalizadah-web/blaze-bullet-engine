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

int positional_value(PieceType type, Color color, Square square, int phase) {
    const int rank = relative_rank(color, square);
    const int center = center_distance(square);
    switch (type) {
        case PieceType::Pawn:
            return rank * 8 - center * 2;
        case PieceType::Knight:
            return 32 - center * 10;
        case PieceType::Bishop:
            return 22 - center * 5 + (rank > 0 ? 6 : 0);
        case PieceType::Rook:
            return rank * 3 - center;
        case PieceType::Queen:
            return 10 - center * 2 - rank * 10 * phase / 24;
        case PieceType::King: {
            int opening = -rank * 18 - center * 3;
            if (rank == 0 && (file_of(square) == 2 || file_of(square) == 6)) {
                opening += 35;
            } else if (rank == 0 && file_of(square) == 4) {
                opening += 10;
            }
            const int endgame = (6 - center) * 8;
            return (opening * phase + endgame * (24 - phase)) / 24;
        }
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
    const Bitboard enemy_pawns = position.pieces(opposite(color), PieceType::Pawn);
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
    constexpr std::array<int, 8> passed_bonus = {0, 5, 10, 20, 35, 60, 100, 0};
    for (int index = 0; index < 64; ++index) {
        const Square square = static_cast<Square>(index);
        if ((pawns & square_bit(square)) == 0) {
            continue;
        }
        bool passed = true;
        for (int enemy_index = 0; enemy_index < 64; ++enemy_index) {
            const Square enemy = static_cast<Square>(enemy_index);
            if ((enemy_pawns & square_bit(enemy)) == 0 ||
                std::abs(file_of(enemy) - file_of(square)) > 1) {
                continue;
            }
            const bool ahead = color == Color::White
                ? rank_of(enemy) > rank_of(square)
                : rank_of(enemy) < rank_of(square);
            if (ahead) {
                passed = false;
                break;
            }
        }
        if (passed) {
            score += passed_bonus[static_cast<std::size_t>(relative_rank(color, square))];
        }
    }
    return score;
}

int game_phase(const Position& position) {
    int phase = 0;
    for (const Color color : {Color::White, Color::Black}) {
        phase += std::popcount(position.pieces(color, PieceType::Knight));
        phase += std::popcount(position.pieces(color, PieceType::Bishop));
        phase += std::popcount(position.pieces(color, PieceType::Rook)) * 2;
        phase += std::popcount(position.pieces(color, PieceType::Queen)) * 4;
    }
    return std::min(phase, 24);
}

int king_shield(const Position& position, Color color, int phase) {
    const Bitboard king = position.pieces(color, PieceType::King);
    if (king == 0) return 0;
    const Square square = static_cast<Square>(std::countr_zero(king));
    const Bitboard pawns = position.pieces(color, PieceType::Pawn);
    int shield = 0;
    const int direction = color == Color::White ? 1 : -1;
    for (int file_delta = -1; file_delta <= 1; ++file_delta) {
        for (int distance = 1; distance <= 2; ++distance) {
            const Square target = make_square(
                file_of(square) + file_delta,
                rank_of(square) + direction * distance);
            if (is_valid_square(target) && (pawns & square_bit(target)) != 0) {
                shield += distance == 1 ? 12 : 5;
            }
        }
    }
    return shield * phase / 24;
}

int rook_files(const Position& position, Color color) {
    int score = 0;
    Bitboard rooks = position.pieces(color, PieceType::Rook);
    const Bitboard friendly_pawns = position.pieces(color, PieceType::Pawn);
    const Bitboard enemy_pawns = position.pieces(opposite(color), PieceType::Pawn);
    while (rooks != 0) {
        const Square square = static_cast<Square>(std::countr_zero(rooks));
        rooks &= rooks - 1;
        const Bitboard file = Bitboard{0x0101010101010101} << file_of(square);
        if ((friendly_pawns & file) == 0) {
            score += (enemy_pawns & file) == 0 ? 18 : 9;
        }
    }
    return score;
}

int score_color(const Position& position, Color color, int phase) {
    int score = 0;
    for (int index = 0; index < 64; ++index) {
        const Square square = static_cast<Square>(index);
        const Piece piece = position.piece_on(square);
        if (piece == Piece::None || color_of(piece) != color) {
            continue;
        }
        const PieceType type = type_of(piece);
        score += material[static_cast<std::size_t>(type)];
        score += positional_value(type, color, square, phase);
    }
    const Bitboard own = color_occupancy(position, color);
    score += mobility(position, color, own);
    score += pawn_structure(position, color);
    score += king_shield(position, color, phase);
    score += rook_files(position, color);
    if (std::popcount(position.pieces(color, PieceType::Bishop)) >= 2) {
        score += 28;
    }
    return score;
}

}  // namespace

int evaluate(const Position& position) {
    Attacks::initialize();
    const int phase = game_phase(position);
    const int white_score = score_color(position, Color::White, phase);
    const int black_score = score_color(position, Color::Black, phase);
    const int relative = position.side_to_move() == Color::White
        ? white_score - black_score
        : black_score - white_score;
    return std::clamp(relative, -search_mate_threshold + 1, search_mate_threshold - 1);
}

}  // namespace blaze
