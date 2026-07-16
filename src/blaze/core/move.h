#ifndef BLAZE_CORE_MOVE_H
#define BLAZE_CORE_MOVE_H

#include "blaze/core/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace blaze {

enum class MoveFlag : std::uint8_t {
    Quiet = 0,
    Capture = 1U << 0U,
    DoublePush = 1U << 1U,
    EnPassant = 1U << 2U,
    CastleKing = 1U << 3U,
    CastleQueen = 1U << 4U,
    Promotion = 1U << 5U,
};

[[nodiscard]] constexpr MoveFlag operator|(MoveFlag left, MoveFlag right) {
    return static_cast<MoveFlag>(
        static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr MoveFlag operator&(MoveFlag left, MoveFlag right) {
    return static_cast<MoveFlag>(
        static_cast<std::uint8_t>(left) & static_cast<std::uint8_t>(right));
}

class Move {
public:
    constexpr Move() = default;

    constexpr Move(
        Square from,
        Square to,
        MoveFlag flags = MoveFlag::Quiet,
        PieceType promotion = PieceType::None)
        : value_(encode(from, to, flags, promotion)) {}

    [[nodiscard]] constexpr bool is_valid() const {
        return value_ != invalid_value;
    }

    [[nodiscard]] constexpr Square from() const {
        return is_valid() ? static_cast<Square>(value_ & 0x3FU) : Square::None;
    }

    [[nodiscard]] constexpr Square to() const {
        return is_valid() ? static_cast<Square>((value_ >> 6U) & 0x3FU) : Square::None;
    }

    [[nodiscard]] constexpr PieceType promotion() const {
        return is_valid()
            ? static_cast<PieceType>((value_ >> 12U) & 0x7U)
            : PieceType::None;
    }

    [[nodiscard]] constexpr MoveFlag flags() const {
        return is_valid()
            ? static_cast<MoveFlag>((value_ >> 16U) & 0xFFU)
            : MoveFlag::Quiet;
    }

    [[nodiscard]] constexpr bool has_flag(MoveFlag flag) const {
        return (static_cast<std::uint8_t>(flags()) & static_cast<std::uint8_t>(flag)) != 0U;
    }

    [[nodiscard]] constexpr std::uint32_t raw() const { return value_; }

    [[nodiscard]] friend constexpr bool operator==(Move, Move) = default;

private:
    static constexpr std::uint32_t invalid_value = 0xFFFFFFFFU;
    std::uint32_t value_ = invalid_value;

    [[nodiscard]] static constexpr std::uint32_t encode(
        Square from,
        Square to,
        MoveFlag flags,
        PieceType promotion) {
        const bool promotion_flag =
            (static_cast<std::uint8_t>(flags) &
             static_cast<std::uint8_t>(MoveFlag::Promotion)) != 0U;
        const bool valid_promotion = promotion == PieceType::Knight ||
                                     promotion == PieceType::Bishop ||
                                     promotion == PieceType::Rook ||
                                     promotion == PieceType::Queen;

        if (!is_valid_square(from) || !is_valid_square(to) || from == to ||
            (promotion_flag != valid_promotion)) {
            return invalid_value;
        }

        return static_cast<std::uint32_t>(square_index(from)) |
               (static_cast<std::uint32_t>(square_index(to)) << 6U) |
               (static_cast<std::uint32_t>(promotion) << 12U) |
               (static_cast<std::uint32_t>(flags) << 16U);
    }
};

[[nodiscard]] Square square_from_string(std::string_view text);
[[nodiscard]] std::string square_to_string(Square square);
[[nodiscard]] std::optional<Move> move_from_uci(std::string_view text);
[[nodiscard]] std::string move_to_uci(Move move);

}  // namespace blaze

#endif  // BLAZE_CORE_MOVE_H
