#include "blaze/search/transposition_table.h"

#include "blaze/eval/classical.h"

#include <algorithm>
#include <limits>

namespace blaze {
namespace {

constexpr std::size_t bytes_per_megabyte = 1024U * 1024U;

}  // namespace

TranspositionTable::TranspositionTable(std::size_t megabytes) {
    resize(megabytes);
}

void TranspositionTable::resize(std::size_t megabytes) {
    const std::size_t bytes = std::max<std::size_t>(megabytes, 1) * bytes_per_megabyte;
    const std::size_t available = std::max<std::size_t>(bytes / sizeof(Cluster), 1);
    std::size_t count = 1;
    while (count <= available / 2) count <<= 1;
    auto replacement = std::make_unique<Cluster[]>(count);

    std::unique_lock lock(table_mutex_);
    clusters_ = std::move(replacement);
    cluster_count_ = count;
    cluster_mask_ = count - 1;
    generation_ = 0;
}

void TranspositionTable::clear() {
    std::shared_lock table_lock(table_mutex_);
    for (std::size_t index = 0; index < cluster_count_; ++index) {
        Cluster& cluster = clusters_[index];
        std::lock_guard cluster_lock(cluster.mutex);
        cluster.entries = {};
    }
}

void TranspositionTable::new_search() {
    std::unique_lock lock(table_mutex_);
    ++generation_;
}

void TranspositionTable::store(
    std::uint64_t key,
    Move move,
    int score,
    int depth,
    Bound bound,
    int ply) {
    std::shared_lock table_lock(table_mutex_);
    Cluster& cluster = clusters_[static_cast<std::size_t>(key) & cluster_mask_];
    std::lock_guard cluster_lock(cluster.mutex);

    Entry* replacement = nullptr;
    int weakest_quality = std::numeric_limits<int>::max();
    for (Entry& entry : cluster.entries) {
        if (!entry.occupied) {
            replacement = &entry;
            break;
        }
        if (entry.key == key) {
            if (depth < entry.depth && !(bound == Bound::Exact && entry.bound != Bound::Exact)) {
                return;
            }
            replacement = &entry;
            break;
        }

        const int age = static_cast<std::uint8_t>(generation_ - entry.generation);
        const int quality = entry.depth - age * 4;
        if (quality < weakest_quality) {
            weakest_quality = quality;
            replacement = &entry;
        }
    }

    *replacement = Entry{
        key,
        move,
        static_cast<std::int16_t>(score_to_table(score, ply)),
        static_cast<std::int16_t>(std::clamp(depth, -32768, 32767)),
        bound,
        generation_,
        true};
}

std::optional<TTData> TranspositionTable::probe(std::uint64_t key, int ply) const {
    std::shared_lock table_lock(table_mutex_);
    const Cluster& cluster = clusters_[static_cast<std::size_t>(key) & cluster_mask_];
    std::lock_guard cluster_lock(cluster.mutex);
    for (const Entry& entry : cluster.entries) {
        if (entry.occupied && entry.key == key) {
            return TTData{
                entry.move,
                score_from_table(entry.score, ply),
                entry.depth,
                entry.bound};
        }
    }
    return std::nullopt;
}

std::size_t TranspositionTable::capacity() const {
    std::shared_lock lock(table_mutex_);
    return cluster_count_ * 4;
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

}  // namespace blaze
