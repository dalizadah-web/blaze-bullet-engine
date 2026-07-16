#ifndef BLAZE_SEARCH_PV_LINE_H
#define BLAZE_SEARCH_PV_LINE_H

#include "blaze/core/move.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace blaze {

class PvLine final {
public:
    static constexpr std::size_t capacity = 128;

    void clear() noexcept {
        size_ = 0;
    }

    void prepend(Move move, const PvLine& child) noexcept {
        moves_[0] = move;
        const std::size_t child_size = std::min(child.size(), capacity - 1U);
        std::copy_n(child.moves_.begin(), child_size, moves_.begin() + 1);
        size_ = static_cast<std::uint8_t>(child_size + 1U);
    }

    [[nodiscard]] std::span<const Move> span() const noexcept {
        return {moves_.data(), size()};
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] Move operator[](std::size_t index) const noexcept {
        return moves_[index];
    }

    [[nodiscard]] Move front() const noexcept {
        return moves_[0];
    }

private:
    std::array<Move, capacity> moves_{};
    std::uint8_t size_ = 0;
};

}  // namespace blaze

#endif  // BLAZE_SEARCH_PV_LINE_H
