#ifndef BLAZE_SEARCH_TRANSPOSITION_TABLE_H
#define BLAZE_SEARCH_TRANSPOSITION_TABLE_H

#include "blaze/core/move.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace blaze {

enum class Bound : std::uint8_t {
    None,
    Upper,
    Lower,
    Exact,
};

struct TTData {
    Move move;
    int score = 0;
    int depth = 0;
    Bound bound = Bound::None;
};

class TranspositionTable {
public:
    explicit TranspositionTable(std::size_t megabytes = 16);

    void resize(std::size_t megabytes);
    void clear();
    void new_search();
    void store(
        std::uint64_t key,
        Move move,
        int score,
        int depth,
        Bound bound,
        int ply);
    [[nodiscard]] std::optional<TTData> probe(std::uint64_t key, int ply) const;
    [[nodiscard]] std::size_t capacity() const;

private:
    struct Entry {
        std::uint64_t key = 0;
        Move move;
        std::int16_t score = 0;
        std::int16_t depth = 0;
        Bound bound = Bound::None;
        std::uint8_t generation = 0;
        bool occupied = false;
    };

    struct Cluster {
        mutable std::mutex mutex;
        std::array<Entry, 4> entries{};
    };

    mutable std::shared_mutex table_mutex_;
    std::unique_ptr<Cluster[]> clusters_;
    std::size_t cluster_count_ = 0;
    std::size_t cluster_mask_ = 0;
    std::uint8_t generation_ = 0;

    [[nodiscard]] static int score_to_table(int score, int ply);
    [[nodiscard]] static int score_from_table(int score, int ply);
};

}  // namespace blaze

#endif  // BLAZE_SEARCH_TRANSPOSITION_TABLE_H
