#ifndef BLAZE_UCI_BOOK_H
#define BLAZE_UCI_BOOK_H

#include "blaze/core/movegen.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace blaze {

enum class BookPolicy : std::uint8_t {
    Best,
    Weighted,
};

class PolyglotBook {
public:
    [[nodiscard]] static std::optional<PolyglotBook> load(
        std::string_view path,
        std::string& error);

    [[nodiscard]] std::optional<Move> select(
        std::uint64_t key,
        const Position& position,
        const MoveList& legal_moves,
        BookPolicy policy,
        std::uint64_t seed) const;

    [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::uint64_t key = 0;
        std::uint16_t move = 0;
        std::uint16_t weight = 0;
    };

    std::vector<Entry> entries_;
};

}  // namespace blaze

#endif  // BLAZE_UCI_BOOK_H
