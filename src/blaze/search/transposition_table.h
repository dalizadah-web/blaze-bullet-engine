#ifndef BLAZE_SEARCH_TRANSPOSITION_TABLE_H
#define BLAZE_SEARCH_TRANSPOSITION_TABLE_H

#include "blaze/core/move.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace blaze {

enum class Bound : std::uint8_t {
    None,
    Upper,
    Lower,
    Exact,
};

inline constexpr int tt_no_static_evaluation = -32768;

struct TTData {
    Move move;
    int score = 0;
    int depth = 0;
    Bound bound = Bound::None;
    int static_evaluation = tt_no_static_evaluation;
    bool pv = false;
    std::uint8_t rule50 = 0;
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
        int ply,
        int rule50,
        int static_evaluation = tt_no_static_evaluation,
        bool pv = false);
    [[nodiscard]] std::optional<TTData> probe(
        std::uint64_t key,
        int ply,
        int rule50 = -1) const noexcept;
    [[nodiscard]] std::size_t capacity() const;
    void prefetch(std::uint64_t key) const noexcept;
    [[nodiscard]] int hashfull() const noexcept;

private:
    struct Snapshot {
        std::uint64_t sequence = 0;
        std::uint64_t key = 0;
        std::uint64_t data0 = 0;
        std::uint64_t data1 = 0;
        bool stable = false;
    };

    struct Entry {
        std::atomic<std::uint64_t> sequence{0};
        std::atomic<std::uint64_t> key{0};
        std::atomic<std::uint64_t> data0{0};
        std::atomic<std::uint64_t> data1{0};
    };

    struct alignas(64) Cluster {
        std::array<Entry, 4> entries{};
    };

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(sizeof(Entry) == 32);
    static_assert(sizeof(Cluster) == 128);
    static_assert(alignof(Cluster) == 64);

    std::unique_ptr<Cluster[]> clusters_;
    std::atomic<Cluster*> cluster_ptr_{nullptr};
    std::atomic<std::size_t> cluster_count_{0};
    std::atomic<std::size_t> cluster_mask_{0};
    std::atomic<unsigned> index_shift_{64};
    std::atomic<std::uint8_t> generation_{0};
    mutable std::mutex lifecycle_mutex_;

    [[nodiscard]] static int score_to_table(int score, int ply);
    [[nodiscard]] static int score_from_table(int score, int ply);
    [[nodiscard]] static std::uint64_t pack_data0(
        Move move,
        int score,
        int static_evaluation) noexcept;
    [[nodiscard]] static std::uint64_t pack_data1(
        int depth,
        int rule50,
        std::uint8_t generation,
        Bound bound,
        bool pv) noexcept;
    [[nodiscard]] static TTData unpack(
        std::uint64_t data0,
        std::uint64_t data1,
        int ply) noexcept;
    [[nodiscard]] static Snapshot read_snapshot(const Entry& entry) noexcept;
    [[nodiscard]] static std::uint8_t generation_from(std::uint64_t data1) noexcept;
    [[nodiscard]] static int depth_from(std::uint64_t data1) noexcept;
    [[nodiscard]] static Bound bound_from(std::uint64_t data1) noexcept;
    [[nodiscard]] static std::uint8_t rule50_from(std::uint64_t data1) noexcept;
    [[nodiscard]] static bool pv_from(std::uint64_t data1) noexcept;
};

}  // namespace blaze

#endif  // BLAZE_SEARCH_TRANSPOSITION_TABLE_H
