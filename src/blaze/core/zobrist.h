#ifndef BLAZE_CORE_ZOBRIST_H
#define BLAZE_CORE_ZOBRIST_H

#include "blaze/core/types.h"

#include <array>
#include <cstdint>

namespace blaze {

class Zobrist final {
public:
    Zobrist() = delete;

    static void initialize();

    [[nodiscard]] static std::uint64_t piece(Piece piece, Square square);
    [[nodiscard]] static std::uint64_t side();
    [[nodiscard]] static std::uint64_t castling(std::uint8_t rights);
    [[nodiscard]] static std::uint64_t ep_file(int file);

private:
    static std::array<std::array<std::uint64_t, 64>, 12> piece_square_;
    static std::array<std::uint64_t, 16> castling_;
    static std::array<std::uint64_t, 8> ep_file_;
    static std::uint64_t side_;
};

}  // namespace blaze

#endif  // BLAZE_CORE_ZOBRIST_H
