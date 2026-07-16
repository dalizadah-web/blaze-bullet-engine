#ifndef BLAZE_CORE_ATTACKS_H
#define BLAZE_CORE_ATTACKS_H

#include "blaze/core/types.h"

#include <array>

namespace blaze {

class Attacks final {
public:
    Attacks() = delete;

    static void initialize();

    [[nodiscard]] static Bitboard pawn(Color color, Square square);
    [[nodiscard]] static Bitboard knight(Square square);
    [[nodiscard]] static Bitboard king(Square square);
    [[nodiscard]] static Bitboard bishop(Square square, Bitboard occupied);
    [[nodiscard]] static Bitboard rook(Square square, Bitboard occupied);
    [[nodiscard]] static Bitboard queen(Square square, Bitboard occupied);

private:
    static std::array<std::array<Bitboard, 64>, 2> pawn_attacks_;
    static std::array<Bitboard, 64> knight_attacks_;
    static std::array<Bitboard, 64> king_attacks_;
};

}  // namespace blaze

#endif  // BLAZE_CORE_ATTACKS_H
