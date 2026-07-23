#include "blaze/search/transposition_table.h"

#include "blaze/eval/classical.h"

#include <algorithm>
#include <limits>

namespace blaze {
namespace {

constexpr std::size_t bytes_per_megabyte = 1024U * 1024U;
constexpr std::uint64_t mask16 = 0xFFFFU;
constexpr std::uint64_t mask8 = 0xFFU;
constexpr std::uint64_t mask2 = 0x3U;

[[nodiscard]] constexpr std::uint64_t encode_signed16(int value, int minimum, int maximum) {
    return static_cast<std::uint64_t>(std::clamp(value, minimum, maximum)) & mask16;
}

[[nodiscard]] constexpr int decode_signed16(std::uint64_t value) {
    const std::uint16_t encoded = static_cast<std::uint16_t>(value & mask16);
    return encoded >= 0x8000U
        ? static_cast<int>(static_cast<std::int16_t>(encoded))
        : static_cast<int>(encoded);
}

}  // namespace

TranspositionTable::TranspositionTable(std::size_t megabytes) {
    resize(megabytes);
}

void TranspositionTable::resize(std::size_t megabytes) {
    const std::size_t bytes = std::max<std::size_t>(megabytes, 1) * bytes_per_megabyte;
    const std::size_t requested = std::max<std::size_t>(bytes / sizeof(Cluster), 1);
    std::size_t count = 1;
    while (count <= requested / 2U) {
        count <<= 1U;
    }
    unsigned bits = 0;
    for (std::size_t value = count; value > 1U; value >>= 1U) {
        ++bits;
    }
    auto replacement = std::make_unique<Cluster[]>(count);

    std::lock_guard lock(lifecycle_mutex_);
    clusters_ = std::move(replacement);
    cluster_count_.store(count, std::memory_order_release);
    cluster_mask_.store(count - 1U, std::memory_order_release);
    index_shift_.store(64U - bits, std::memory_order_release);
    generation_.store(0, std::memory_order_release);
    cluster_ptr_.store(clusters_.get(), std::memory_order_release);
}

void TranspositionTable::clear() {
    std::lock_guard lock(lifecycle_mutex_);
    Cluster* const clusters = cluster_ptr_.load(std::memory_order_acquire);
    const std::size_t count = cluster_count_.load(std::memory_order_acquire);
    for (std::size_t index = 0; index < count; ++index) {
        for (Entry& entry : clusters[index].entries) {
            entry.data0.store(0, std::memory_order_relaxed);
            entry.data1.store(0, std::memory_order_relaxed);
            entry.key.store(0, std::memory_order_relaxed);
            entry.sequence.store(0, std::memory_order_release);
        }
    }
}

void TranspositionTable::new_search() {
    generation_.fetch_add(1, std::memory_order_relaxed);
}

void TranspositionTable::store(
    std::uint64_t key,
    Move move,
    int score,
    int depth,
    Bound bound,
    int ply,
    int rule50,
    int static_evaluation,
    bool pv) {
    Cluster* const clusters = cluster_ptr_.load(std::memory_order_acquire);
    const std::size_t count = cluster_count_.load(std::memory_order_relaxed);
    const std::size_t mask = cluster_mask_.load(std::memory_order_relaxed);
    const unsigned shift = index_shift_.load(std::memory_order_relaxed);
    if (clusters == nullptr || count == 0U) {
        return;
    }
    const std::size_t index = count == 1U ? 0U : ((key >> shift) & mask);
    Cluster& cluster = clusters[index];
    const std::uint8_t generation = generation_.load(std::memory_order_relaxed);
    const std::uint8_t stored_rule50 = static_cast<std::uint8_t>(std::clamp(rule50, 0, 100));

    int same_key_slot = -1;
    Snapshot same_key_snapshot;
    int empty = -1;
    Snapshot empty_snapshot;
    int weakest_slot = -1;
    Snapshot weakest_snapshot;
    int weakest_quality = std::numeric_limits<int>::max();
    for (int slot = 0; slot < static_cast<int>(cluster.entries.size()); ++slot) {
        const Snapshot snapshot = read_snapshot(cluster.entries[static_cast<std::size_t>(slot)]);
        if (!snapshot.stable) {
            continue;
        }
        const Bound old_bound = bound_from(snapshot.data1);
        if (snapshot.key == key && old_bound != Bound::None) {
            same_key_slot = slot;
            same_key_snapshot = snapshot;
            break;
        }
        if (old_bound == Bound::None) {
            if (empty < 0) {
                empty = slot;
                empty_snapshot = snapshot;
            }
            continue;
        }
        const int age = static_cast<std::uint8_t>(generation - generation_from(snapshot.data1));
        const int quality = depth_from(snapshot.data1) +
            (old_bound == Bound::Exact ? 4 : 0) + (pv_from(snapshot.data1) ? 2 : 0) - age * 4;
        if (quality < weakest_quality) {
            weakest_quality = quality;
            weakest_slot = slot;
            weakest_snapshot = snapshot;
        }
    }
    int chosen = same_key_slot;
    Snapshot chosen_snapshot = same_key_snapshot;
    if (chosen < 0 && empty >= 0) {
        chosen = empty;
        chosen_snapshot = empty_snapshot;
    } else if (chosen < 0) {
        chosen = weakest_slot;
        chosen_snapshot = weakest_snapshot;
    }
    if (chosen < 0) {
        return;
    }

    const Bound old_bound = bound_from(chosen_snapshot.data1);
    const bool same_key = chosen_snapshot.stable && chosen_snapshot.key == key &&
        old_bound != Bound::None;
    const bool same_rule50 = same_key &&
        rule50_from(chosen_snapshot.data1) == stored_rule50;
    if (same_rule50 && depth < depth_from(chosen_snapshot.data1) &&
        !(bound == Bound::Exact && old_bound != Bound::Exact)) {
        return;
    }

    const std::uint64_t packed_data0 = pack_data0(move, score_to_table(score, ply), static_evaluation);
    const std::uint64_t packed_data1 = pack_data1(depth, stored_rule50, generation, bound, pv);
    const auto publish = [&](int slot, const Snapshot& snapshot) {
        Entry& entry = cluster.entries[static_cast<std::size_t>(slot)];
        std::uint64_t expected = snapshot.sequence;
        if ((expected & 1U) != 0U ||
            !entry.sequence.compare_exchange_strong(
                expected,
                expected + 1U,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return false;
        }
        entry.key.store(key, std::memory_order_relaxed);
        entry.data0.store(packed_data0, std::memory_order_relaxed);
        entry.data1.store(packed_data1, std::memory_order_relaxed);
        entry.sequence.store(snapshot.sequence + 2U, std::memory_order_release);
        return true;
    };
    if (publish(chosen, chosen_snapshot)) {
        return;
    }
    // A preempted writer must not make a TT store wait. Dropping this hint is
    // preferable to evicting a newly published deep/exact/PV entry blindly.
}

std::optional<TTData> TranspositionTable::probe(
    std::uint64_t key,
    int ply,
    int rule50) const noexcept {
    Cluster* const clusters = cluster_ptr_.load(std::memory_order_acquire);
    const std::size_t count = cluster_count_.load(std::memory_order_relaxed);
    const std::size_t mask = cluster_mask_.load(std::memory_order_relaxed);
    const unsigned shift = index_shift_.load(std::memory_order_relaxed);
    if (clusters == nullptr || count == 0U) {
        return std::nullopt;
    }
    const std::size_t index = count == 1U ? 0U : ((key >> shift) & mask);
    const Cluster& cluster = clusters[index];
    const std::uint8_t requested_rule50 = static_cast<std::uint8_t>(
        std::clamp(rule50, 0, 100));
    const bool require_exact_rule50 = rule50 >= 0;
    const std::uint8_t generation = generation_.load(std::memory_order_relaxed);
    std::optional<TTData> best;
    int best_quality = std::numeric_limits<int>::min();
    bool best_rule50_exact = false;
    for (const Entry& entry : cluster.entries) {
        const std::uint64_t first_sequence = entry.sequence.load(std::memory_order_acquire);
        if ((first_sequence & 1U) != 0U) {
            continue;
        }
        const std::uint64_t observed_key = entry.key.load(std::memory_order_relaxed);
        if (observed_key != key) {
            continue;
        }
        const std::uint64_t data0 = entry.data0.load(std::memory_order_relaxed);
        const std::uint64_t data1 = entry.data1.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint64_t second_sequence = entry.sequence.load(std::memory_order_acquire);
        if (first_sequence != second_sequence || (second_sequence & 1U) != 0U) {
            continue;
        }
        const Snapshot snapshot{second_sequence, observed_key, data0, data1, true};
        const Bound bound = bound_from(snapshot.data1);
        if (bound == Bound::None) {
            continue;
        }
        const bool exact_rule50 = !require_exact_rule50 ||
            rule50_from(snapshot.data1) == requested_rule50;
        const int age = static_cast<std::uint8_t>(generation - generation_from(snapshot.data1));
        const int quality = depth_from(snapshot.data1) +
            (bound == Bound::Exact ? 4 : 0) + (pv_from(snapshot.data1) ? 2 : 0) - age * 4;
        if (!best.has_value() || (exact_rule50 && !best_rule50_exact) ||
            (exact_rule50 == best_rule50_exact && quality > best_quality)) {
            best = unpack(snapshot.data0, snapshot.data1, ply);
            best_quality = quality;
            best_rule50_exact = exact_rule50;
        }
    }
    return best;
}

std::size_t TranspositionTable::capacity() const {
    return cluster_count_.load(std::memory_order_acquire) * 4U;
}

void TranspositionTable::prefetch(std::uint64_t key) const noexcept {
    Cluster* const clusters = cluster_ptr_.load(std::memory_order_relaxed);
    const std::size_t count = cluster_count_.load(std::memory_order_relaxed);
    const std::size_t mask = cluster_mask_.load(std::memory_order_relaxed);
    const unsigned shift = index_shift_.load(std::memory_order_relaxed);
    if (clusters == nullptr || count == 0U) {
        return;
    }
    const std::size_t index = count == 1U ? 0U : ((key >> shift) & mask);
#if defined(__GNUC__) || defined(__clang__)
    const char* const address = reinterpret_cast<const char*>(&clusters[index]);
    __builtin_prefetch(address, 0, 1);
    __builtin_prefetch(address + 64, 0, 1);
#else
    (void)index;
#endif
}

int TranspositionTable::hashfull() const noexcept {
    Cluster* const clusters = cluster_ptr_.load(std::memory_order_acquire);
    const std::size_t count = cluster_count_.load(std::memory_order_acquire);
    if (clusters == nullptr || count == 0U) {
        return 0;
    }
    const std::size_t sample_clusters = std::min<std::size_t>(count, 1000U);
    const std::uint8_t generation = generation_.load(std::memory_order_relaxed);
    std::size_t occupied = 0;
    for (std::size_t index = 0; index < sample_clusters; ++index) {
        for (const Entry& entry : clusters[index].entries) {
            const Snapshot snapshot = read_snapshot(entry);
            if (snapshot.stable && bound_from(snapshot.data1) != Bound::None &&
                generation_from(snapshot.data1) == generation) {
                ++occupied;
            }
        }
    }
    return static_cast<int>((occupied * 1000U) / (sample_clusters * 4U));
}

int TranspositionTable::score_to_table(int score, int ply) {
    if (score >= search_mate_threshold) {
        return score + ply;
    }
    if (score <= -search_mate_threshold) {
        return score - ply;
    }
    return score;
}

int TranspositionTable::score_from_table(int score, int ply) {
    if (score >= search_mate_threshold) {
        return score - ply;
    }
    if (score <= -search_mate_threshold) {
        return score + ply;
    }
    return score;
}

std::uint64_t TranspositionTable::pack_data0(
    Move move,
    int score,
    int static_evaluation) noexcept {
    const int stored_static_evaluation = static_evaluation == tt_no_static_evaluation
        ? tt_no_static_evaluation
        : std::clamp(static_evaluation, -32767, 32767);
    return static_cast<std::uint64_t>(move.raw()) |
           (encode_signed16(score, -32768, 32767) << 32U) |
           (encode_signed16(stored_static_evaluation, -32768, 32767) << 48U);
}

std::uint64_t TranspositionTable::pack_data1(
    int depth,
    int rule50,
    std::uint8_t generation,
    Bound bound,
    bool pv) noexcept {
    return encode_signed16(depth, -32768, 32767) |
           ((static_cast<std::uint64_t>(std::clamp(rule50, 0, 100)) & mask8) << 16U) |
           (static_cast<std::uint64_t>(generation) << 24U) |
           ((static_cast<std::uint64_t>(bound) & mask2) << 32U) |
           (static_cast<std::uint64_t>(pv ? 1U : 0U) << 34U);
}

TTData TranspositionTable::unpack(
    std::uint64_t data0,
    std::uint64_t data1,
    int ply) noexcept {
    const std::uint32_t raw_move = static_cast<std::uint32_t>(data0 & 0xFFFFFFFFU);
    Move move;
    if (raw_move != 0xFFFFFFFFU) {
        move = Move(
            static_cast<Square>(raw_move & 0x3FU),
            static_cast<Square>((raw_move >> 6U) & 0x3FU),
            static_cast<MoveFlag>((raw_move >> 16U) & 0x3FU),
            static_cast<PieceType>((raw_move >> 12U) & 0x7U));
    }
    return TTData{
        move,
        score_from_table(decode_signed16(data0 >> 32U), ply),
        decode_signed16(data1),
        bound_from(data1),
        decode_signed16(data0 >> 48U),
        pv_from(data1),
        rule50_from(data1)};
}

TranspositionTable::Snapshot TranspositionTable::read_snapshot(const Entry& entry) noexcept {
    const std::uint64_t first = entry.sequence.load(std::memory_order_acquire);
    if ((first & 1U) != 0U) {
        return Snapshot{first, 0, 0, 0, false};
    }
    const std::uint64_t key = entry.key.load(std::memory_order_relaxed);
    const std::uint64_t data0 = entry.data0.load(std::memory_order_relaxed);
    const std::uint64_t data1 = entry.data1.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint64_t second = entry.sequence.load(std::memory_order_acquire);
    return Snapshot{second, key, data0, data1, first == second && (second & 1U) == 0U};
}

std::uint8_t TranspositionTable::generation_from(std::uint64_t data1) noexcept {
    return static_cast<std::uint8_t>((data1 >> 24U) & mask8);
}

int TranspositionTable::depth_from(std::uint64_t data1) noexcept {
    return decode_signed16(data1);
}

Bound TranspositionTable::bound_from(std::uint64_t data1) noexcept {
    return static_cast<Bound>((data1 >> 32U) & mask2);
}

std::uint8_t TranspositionTable::rule50_from(std::uint64_t data1) noexcept {
    return static_cast<std::uint8_t>((data1 >> 16U) & mask8);
}

bool TranspositionTable::pv_from(std::uint64_t data1) noexcept {
    return ((data1 >> 34U) & 1U) != 0U;
}

}  // namespace blaze
