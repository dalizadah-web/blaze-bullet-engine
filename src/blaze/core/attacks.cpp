#include "blaze/core/attacks.h"

#include <array>
#include <mutex>
#include <utility>

namespace blaze {

std::array<std::array<Bitboard, 64>, 2> Attacks::pawn_attacks_{};
std::array<Bitboard, 64> Attacks::knight_attacks_{};
std::array<Bitboard, 64> Attacks::king_attacks_{};

namespace {

std::once_flag initialization_flag;

[[nodiscard]] Bitboard bit_at(int file, int rank) {
    const Square square = make_square(file, rank);
    return is_valid_square(square)
        ? (Bitboard{1} << static_cast<unsigned>(square_index(square)))
        : Bitboard{0};
}

template <std::size_t Size>
[[nodiscard]] Bitboard generated_steps(
    Square square,
    const std::array<std::pair<int, int>, Size>& steps) {
    Bitboard result = 0;
    for (const auto [file_delta, rank_delta] : steps) {
        result |= bit_at(file_of(square) + file_delta, rank_of(square) + rank_delta);
    }
    return result;
}

[[nodiscard]] Bitboard ray(
    Square square,
    Bitboard occupied,
    int file_delta,
    int rank_delta) {
    Bitboard result = 0;
    int file = file_of(square) + file_delta;
    int rank = rank_of(square) + rank_delta;

    while (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
        const Square target = make_square(file, rank);
        const Bitboard target_bit = Bitboard{1} << static_cast<unsigned>(square_index(target));
        result |= target_bit;
        if ((occupied & target_bit) != 0) {
            break;
        }
        file += file_delta;
        rank += rank_delta;
    }
    return result;
}

}  // namespace

void Attacks::initialize() {
    std::call_once(initialization_flag, [] {
        constexpr std::array<std::pair<int, int>, 8> knight_steps{{
            {-2, -1}, {-2, 1}, {-1, -2}, {-1, 2},
            {1, -2}, {1, 2}, {2, -1}, {2, 1}}};
        constexpr std::array<std::pair<int, int>, 8> king_steps{{
            {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
            {1, 0}, {-1, 1}, {0, 1}, {1, 1}}};

        for (int index = 0; index < 64; ++index) {
            const Square square = static_cast<Square>(index);
            const int file = file_of(square);
            const int rank = rank_of(square);

            pawn_attacks_[static_cast<std::size_t>(Color::White)][index] =
                bit_at(file - 1, rank + 1) | bit_at(file + 1, rank + 1);
            pawn_attacks_[static_cast<std::size_t>(Color::Black)][index] =
                bit_at(file - 1, rank - 1) | bit_at(file + 1, rank - 1);
            knight_attacks_[index] = generated_steps(square, knight_steps);
            king_attacks_[index] = generated_steps(square, king_steps);
        }
    });
}

Bitboard Attacks::pawn(Color color, Square square) {
    initialize();
    if (!is_valid_square(square)) {
        return 0;
    }
    return pawn_attacks_[static_cast<std::size_t>(color)][square_index(square)];
}

Bitboard Attacks::knight(Square square) {
    initialize();
    return is_valid_square(square) ? knight_attacks_[square_index(square)] : 0;
}

Bitboard Attacks::king(Square square) {
    initialize();
    return is_valid_square(square) ? king_attacks_[square_index(square)] : 0;
}

Bitboard Attacks::bishop(Square square, Bitboard occupied) {
    if (!is_valid_square(square)) {
        return 0;
    }
    return ray(square, occupied, 1, 1) |
           ray(square, occupied, -1, 1) |
           ray(square, occupied, 1, -1) |
           ray(square, occupied, -1, -1);
}

Bitboard Attacks::rook(Square square, Bitboard occupied) {
    if (!is_valid_square(square)) {
        return 0;
    }
    return ray(square, occupied, 1, 0) |
           ray(square, occupied, -1, 0) |
           ray(square, occupied, 0, 1) |
           ray(square, occupied, 0, -1);
}

Bitboard Attacks::queen(Square square, Bitboard occupied) {
    return bishop(square, occupied) | rook(square, occupied);
}

}  // namespace blaze
