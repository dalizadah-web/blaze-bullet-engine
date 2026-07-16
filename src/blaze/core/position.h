#ifndef BLAZE_CORE_POSITION_H
#define BLAZE_CORE_POSITION_H

#include "blaze/core/move.h"
#include "blaze/core/types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace blaze {

enum class CastlingRight : std::uint8_t {
    WhiteKing = 1U << 0U,
    WhiteQueen = 1U << 1U,
    BlackKing = 1U << 2U,
    BlackQueen = 1U << 3U,
};

using CastlingRights = std::uint8_t;

template <typename... Rights>
[[nodiscard]] constexpr CastlingRights castling_mask(Rights... rights) {
    return (CastlingRights{0} | ... | static_cast<CastlingRights>(rights));
}

[[nodiscard]] constexpr bool has_castling(
    CastlingRights rights,
    CastlingRight right) {
    return (rights & static_cast<CastlingRights>(right)) != 0U;
}

struct StateInfo {
    Color side_to_move = Color::White;
    CastlingRights castling_rights = 0;
    Square ep_square = Square::None;
    int rule50 = 0;
    int fullmove_number = 1;
    std::uint64_t key = 0;
    Piece captured_piece = Piece::None;
    Square captured_square = Square::None;
};

class Position {
public:
    [[nodiscard]] static std::optional<Position> from_fen(std::string_view fen);

    [[nodiscard]] std::string to_fen() const;
    [[nodiscard]] Piece piece_on(Square square) const;
    [[nodiscard]] Color side_to_move() const { return side_to_move_; }
    [[nodiscard]] CastlingRights castling_rights() const { return castling_rights_; }
    [[nodiscard]] Square ep_square() const { return ep_square_; }
    [[nodiscard]] int rule50() const { return rule50_; }
    [[nodiscard]] int fullmove_number() const { return fullmove_number_; }
    [[nodiscard]] Bitboard occupied() const { return occupied_; }
    [[nodiscard]] Bitboard pieces(Color color, PieceType type) const;
    [[nodiscard]] bool is_consistent() const;
    [[nodiscard]] std::uint64_t key() const { return key_; }

    bool make_move(Move move, StateInfo& state);
    void unmake_move(Move move, const StateInfo& state);
    void make_null(StateInfo& state);
    void unmake_null(const StateInfo& state);
    [[nodiscard]] bool is_legal(Move move);

private:
    std::array<Bitboard, 12> piece_bitboards_{};
    std::array<Piece, 64> board_{};
    Bitboard occupied_ = 0;
    Color side_to_move_ = Color::White;
    CastlingRights castling_rights_ = 0;
    Square ep_square_ = Square::None;
    int rule50_ = 0;
    int fullmove_number_ = 1;
    std::uint64_t key_ = 0;

    bool put_piece(Piece piece, Square square);
    void add_piece(Piece piece, Square square);
    Piece remove_piece(Square square);
    void relocate_piece(Square from, Square to);
    [[nodiscard]] bool ep_is_effective() const;
    [[nodiscard]] std::uint64_t recompute_key() const;
    void update_castling_rights(Piece mover, Square from, Piece captured, Square captured_square);
};

}  // namespace blaze

#endif  // BLAZE_CORE_POSITION_H
