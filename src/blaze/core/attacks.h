#ifndef BLAZE_CORE_ATTACKS_H
#define BLAZE_CORE_ATTACKS_H

#include "blaze/core/types.h"

#include <array>

namespace blaze {

class Attacks final {
public:
    Attacks() = delete;

    struct FixedAttackerMasks {
        Bitboard pawns = 0;
        Bitboard knights = 0;
        Bitboard kings = 0;
    };

    static void initialize();

    [[nodiscard]] static Bitboard pawn(Color color, Square square);
    [[nodiscard]] static Bitboard knight(Square square);
    [[nodiscard]] static Bitboard king(Square square);
    [[nodiscard]] static Bitboard bishop(Square square, Bitboard occupied);
    [[nodiscard]] static Bitboard rook(Square square, Bitboard occupied);
    [[nodiscard]] static Bitboard queen(Square square, Bitboard occupied);
    // Masks of pieces of `attacker` that can attack `target` without occupancy.
    [[nodiscard]] static FixedAttackerMasks fixed_attacker_masks(Color attacker, Square target);

private:
    static std::array<std::array<Bitboard, 64>, 2> pawn_attacks_;
    static std::array<Bitboard, 64> knight_attacks_;
    static std::array<Bitboard, 64> king_attacks_;
};

}  // namespace blaze

#endif  // BLAZE_CORE_ATTACKS_H
