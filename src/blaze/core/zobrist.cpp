#include "blaze/core/zobrist.h"

#include <cstddef>
#include <mutex>

namespace blaze {

std::array<std::array<std::uint64_t, 64>, 12> Zobrist::piece_square_{};
std::array<std::uint64_t, 16> Zobrist::castling_{};
std::array<std::uint64_t, 8> Zobrist::ep_file_{};
std::uint64_t Zobrist::side_ = 0;

namespace {

std::once_flag initialization_flag;

class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

    [[nodiscard]] std::uint64_t next() {
        std::uint64_t value = (state_ += 0x9E3779B97F4A7C15ULL);
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }

private:
    std::uint64_t state_;
};

}  // namespace

void Zobrist::initialize() {
    std::call_once(initialization_flag, [] {
        SplitMix64 generator{0xB1A2E5EED2026071ULL};
        for (auto& piece_table : piece_square_) {
            for (std::uint64_t& key : piece_table) {
                key = generator.next();
            }
        }
        for (std::uint64_t& key : castling_) {
            key = generator.next();
        }
        for (std::uint64_t& key : ep_file_) {
            key = generator.next();
        }
        side_ = generator.next();
    });
}

std::uint64_t Zobrist::piece(Piece piece_value, Square square) {
    initialize();
    return piece_square_[static_cast<std::size_t>(piece_index(piece_value))]
                        [static_cast<std::size_t>(square_index(square))];
}

std::uint64_t Zobrist::side() {
    initialize();
    return side_;
}

std::uint64_t Zobrist::castling(std::uint8_t rights) {
    initialize();
    return castling_[rights & 0x0FU];
}

std::uint64_t Zobrist::ep_file(int file) {
    initialize();
    return ep_file_[static_cast<std::size_t>(file)];
}

}  // namespace blaze
