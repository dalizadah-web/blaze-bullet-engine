#include "blaze/search/see.h"

#include "blaze/core/attacks.h"
#include "blaze/core/movegen.h"

#include <array>
#include <bit>

namespace blaze {
namespace {

constexpr std::array<int, 7> piece_values = {0, 100, 320, 335, 500, 900, 20'000};

int value(Piece piece) {
    return piece_values[static_cast<std::size_t>(type_of(piece))];
}

bool king_safe_after(Position& position, Color moving_side) {
    const Bitboard king = position.pieces(moving_side, PieceType::King);
    if (king == 0) return false;
    const Square square = static_cast<Square>(std::countr_zero(king));
    return !is_square_attacked(position, square, opposite(moving_side));
}

Bitboard attackers(const Position& position, Square target, Color color, PieceType type) {
    switch (type) {
        case PieceType::Pawn:
            return Attacks::pawn(opposite(color), target) & position.pieces(color, type);
        case PieceType::Knight:
            return Attacks::knight(target) & position.pieces(color, type);
        case PieceType::Bishop:
            return Attacks::bishop(target, position.occupied()) & position.pieces(color, type);
        case PieceType::Rook:
            return Attacks::rook(target, position.occupied()) & position.pieces(color, type);
        case PieceType::Queen:
            return Attacks::queen(target, position.occupied()) & position.pieces(color, type);
        case PieceType::King:
            return Attacks::king(target) & position.pieces(color, type);
        case PieceType::None:
            return 0;
    }
    return 0;
}

int exchange(Position& position, Square target, Color side) {
    const Piece victim = position.piece_on(target);
    if (victim == Piece::None || type_of(victim) == PieceType::King) return 0;

    for (const PieceType type : {PieceType::Pawn, PieceType::Knight, PieceType::Bishop,
                                 PieceType::Rook, PieceType::Queen, PieceType::King}) {
        Bitboard candidates = attackers(position, target, side, type);
        while (candidates != 0) {
            const Square from = static_cast<Square>(std::countr_zero(candidates));
            candidates &= candidates - 1;
            if (type == PieceType::Pawn && (rank_of(target) == 0 || rank_of(target) == 7)) {
                int best = 0;
                bool found_legal = false;
                for (const PieceType promotion : {PieceType::Queen, PieceType::Rook,
                                                  PieceType::Bishop, PieceType::Knight}) {
                    StateInfo state;
                    const Move move{
                        from,
                        target,
                        MoveFlag::Capture | MoveFlag::Promotion,
                        promotion};
                    if (!position.make_move(move, state)) continue;
                    const bool legal = king_safe_after(position, side);
                    if (!legal) {
                        position.unmake_move(move, state);
                        continue;
                    }
                    found_legal = true;
                    const int promotion_gain = value(make_piece(side, promotion)) -
                        value(make_piece(side, PieceType::Pawn));
                    const int reply = exchange(position, target, opposite(side));
                    position.unmake_move(move, state);
                    best = std::max(best, value(victim) + promotion_gain - reply);
                }
                if (found_legal) return best;
                continue;
            }
            StateInfo state;
            const Move move{from, target, MoveFlag::Capture};
            if (!position.make_move(move, state)) continue;
            const bool legal = king_safe_after(position, side);
            if (!legal) {
                position.unmake_move(move, state);
                continue;
            }
            const int reply = exchange(position, target, opposite(side));
            position.unmake_move(move, state);
            return std::max(0, value(victim) - reply);
        }
    }
    return 0;
}

}  // namespace

int static_exchange_evaluation(const Position& position, Move move) {
    if (!move.is_valid() ||
        (!move.has_flag(MoveFlag::Capture) && !move.has_flag(MoveFlag::EnPassant))) {
        return 0;
    }
    const Piece victim = move.has_flag(MoveFlag::EnPassant)
        ? make_piece(opposite(position.side_to_move()), PieceType::Pawn)
        : position.piece_on(move.to());
    if (victim == Piece::None || type_of(victim) == PieceType::King) return 0;

    Position after = position;
    StateInfo state;
    if (!after.make_move(move, state) || !king_safe_after(after, position.side_to_move())) {
        return -20'000;
    }
    const int promotion_gain = move.has_flag(MoveFlag::Promotion)
        ? value(make_piece(position.side_to_move(), move.promotion())) - value(make_piece(position.side_to_move(), PieceType::Pawn))
        : 0;
    const int reply = exchange(after, move.to(), opposite(position.side_to_move()));
    return value(victim) + promotion_gain - reply;
}

}  // namespace blaze
