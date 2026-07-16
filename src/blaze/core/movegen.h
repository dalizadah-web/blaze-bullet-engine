#ifndef BLAZE_CORE_MOVEGEN_H
#define BLAZE_CORE_MOVEGEN_H

#include "blaze/core/move.h"
#include "blaze/core/position.h"

#include <array>
#include <cassert>
#include <cstddef>

namespace blaze {

class MoveList {
public:
    static constexpr std::size_t capacity = 256;

    void clear() { size_ = 0; }

    void push(Move move) {
        assert(size_ < capacity);
        moves_[size_++] = move;
    }

    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] const Move* begin() const { return moves_.data(); }
    [[nodiscard]] const Move* end() const { return moves_.data() + size_; }
    [[nodiscard]] Move operator[](std::size_t index) const {
        assert(index < size_);
        return moves_[index];
    }
    [[nodiscard]] Move& operator[](std::size_t index) {
        assert(index < size_);
        return moves_[index];
    }

private:
    std::array<Move, capacity> moves_{};
    std::size_t size_ = 0;
};

void generate_pseudo_legal(const Position& position, MoveList& moves);
void generate_legal(Position& position, MoveList& moves);
[[nodiscard]] bool is_square_attacked(const Position& position, Square square, Color by_color);
[[nodiscard]] bool in_check(const Position& position);

}  // namespace blaze

#endif  // BLAZE_CORE_MOVEGEN_H
