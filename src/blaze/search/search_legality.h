#ifndef BLAZE_SEARCH_SEARCH_LEGALITY_H
#define BLAZE_SEARCH_SEARCH_LEGALITY_H

#include "blaze/core/movegen.h"

#include <bit>

namespace blaze {

[[nodiscard]] inline bool last_move_kept_own_king_safe(const Position& position) {
    const Color moving_side = opposite(position.side_to_move());
    const Bitboard king = position.pieces(moving_side, PieceType::King);
    if (king == 0) return false;
    const Square king_square = static_cast<Square>(std::countr_zero(king));
    return !is_square_attacked(position, king_square, position.side_to_move());
}

}  // namespace blaze

#endif  // BLAZE_SEARCH_SEARCH_LEGALITY_H
