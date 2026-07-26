#include "blaze/eval/network.h"

#include "blaze/core/attacks.h"
#include "blaze/eval/classical.h"
#include "blaze/eval/nnue_kernels.h"

#include "misc.h"
#include "nnue/features/full_threats.h"
#include "nnue/features/half_ka_v2_hm.h"
#include "nnue/network.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace blaze {
namespace {

using Stockfish::Eval::NNUE::BigFeatureTransformer;
using Stockfish::Eval::NNUE::NetworkBig;
constexpr std::size_t kDimensions = 1024;
constexpr std::size_t kPlyCapacity = 132;
constexpr std::size_t kBuckets = 8;

struct AtomicNnueRuntimeStats {
    std::atomic<std::uint64_t> network_loads{0};
    std::atomic<std::uint64_t> thread_state_constructions{0};
    std::atomic<std::uint64_t> root_tasks{0};
    std::atomic<std::uint64_t> root_task_state_constructions{0};
    std::atomic<std::uint64_t> hot_path_heap_allocations{0};
    std::atomic<std::uint64_t> fen_serializations{0};
    std::atomic<std::uint64_t> stockfish_position_constructions{0};
    std::atomic<std::uint64_t> accumulator_refreshes{0};
    std::atomic<std::uint64_t> refresh_cache_hits{0};
    std::atomic<std::uint64_t> incremental_updates{0};
    std::atomic<std::uint64_t> evaluations{0};
};

AtomicNnueRuntimeStats g_runtime_stats;
thread_local unsigned g_root_task_depth = 0;

#if defined(BLAZE_NNUE_BENCHMARK) || !defined(NDEBUG)
struct AtomicProfileComponent {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> sampled_calls{0};
    std::atomic<std::uint64_t> sampled_nanoseconds{0};
};

struct AtomicBenchmarkStats {
    std::atomic<std::uint64_t> scalar_kernel_calls{0};
    std::atomic<std::uint64_t> avx2_kernel_calls{0};
    std::atomic<std::uint64_t> fresh_evaluations{0};
    std::atomic<std::uint64_t> incremental_evaluations{0};
    std::atomic<std::uint64_t> full_refreshes{0};
    std::atomic<std::uint64_t> inferences{0};
    std::atomic<std::uint64_t> refresh_cache_lookups{0};
    std::atomic<std::uint64_t> refresh_cache_hits{0};
    std::atomic<std::uint64_t> refresh_cache_misses{0};
    std::atomic<std::uint64_t> refresh_cache_stores{0};
    std::atomic<std::uint64_t> refresh_cache_replacements{0};
    std::atomic<std::uint64_t> refresh_cache_invalidations{0};
    std::atomic<std::uint64_t> refresh_cache_uninitialized_misses{0};
    std::atomic<std::uint64_t> refresh_cache_key_misses{0};
    std::atomic<std::uint64_t> refresh_cache_hit_bytes{0};
    std::array<std::array<std::atomic<std::uint64_t>, 8>, 2> refresh_cache_hits_by_perspective_bucket{};
    std::atomic<std::uint64_t> king_bucket_refreshes{0};
    std::array<std::atomic<std::uint64_t>, 3> halfka_removed_features{};
    std::array<std::atomic<std::uint64_t>, 3> halfka_added_features{};
    std::array<std::atomic<std::uint64_t>, 97> full_threats_removed_features{};
    std::array<std::atomic<std::uint64_t>, 97> full_threats_added_features{};
    std::array<std::atomic<std::uint64_t>, 197> total_dirty_rows{};
    std::array<std::atomic<std::uint64_t>, 7> delta_move_types{};
    std::atomic<std::uint64_t> accumulator_full_passes{0};
    std::atomic<std::uint64_t> accumulator_bytes_read{0};
    std::atomic<std::uint64_t> accumulator_bytes_written{0};
    std::array<AtomicProfileComponent,
               static_cast<std::size_t>(NnueProfileComponent::Count)> components{};
};

AtomicBenchmarkStats g_benchmark_stats;
[[maybe_unused]] constexpr std::uint64_t kProfileSampleMask = 63;

bool profile_component_should_sample(NnueProfileComponent component) noexcept {
    const std::size_t index = static_cast<std::size_t>(component);
    const std::uint64_t ticket = g_benchmark_stats.components[index].calls.fetch_add(
        1, std::memory_order_relaxed);
#if defined(BLAZE_NNUE_BENCHMARK)
    return (ticket & kProfileSampleMask) == 0;
#else
    static_cast<void>(ticket);
    return false;
#endif
}

void profile_component_sample(NnueProfileComponent component, std::uint64_t nanoseconds) noexcept {
#if defined(BLAZE_NNUE_BENCHMARK)
    AtomicProfileComponent& entry =
        g_benchmark_stats.components[static_cast<std::size_t>(component)];
    entry.sampled_calls.fetch_add(1, std::memory_order_relaxed);
    entry.sampled_nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
#else
    static_cast<void>(component);
    static_cast<void>(nanoseconds);
#endif
}

class ProfileScope final {
public:
    explicit ProfileScope(NnueProfileComponent component) noexcept : component_(component),
        sampled_(profile_component_should_sample(component)) {
#if defined(BLAZE_NNUE_BENCHMARK)
        if (sampled_) start_ = std::chrono::steady_clock::now();
#endif
    }
    ~ProfileScope() {
#if defined(BLAZE_NNUE_BENCHMARK)
        if (sampled_) {
            const auto elapsed = std::chrono::steady_clock::now() - start_;
            profile_component_sample(component_, static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
        }
#endif
    }
private:
    [[maybe_unused]] NnueProfileComponent component_;
    bool sampled_;
#if defined(BLAZE_NNUE_BENCHMARK)
    std::chrono::steady_clock::time_point start_{};
#endif
};

void record_kernel_call(bool avx2) noexcept {
    (avx2 ? g_benchmark_stats.avx2_kernel_calls : g_benchmark_stats.scalar_kernel_calls)
        .fetch_add(1, std::memory_order_relaxed);
}

enum class BenchmarkCounter {
    Fresh, Incremental, Refresh, Inference, CacheLookup, CacheHit, CacheMiss,
    CacheStore, CacheReplacement, CacheInvalidation, CacheUninitializedMiss, CacheKeyMiss, KingRefresh
};
void record_benchmark_counter(BenchmarkCounter counter) noexcept {
    switch (counter) {
        case BenchmarkCounter::Fresh: ++g_benchmark_stats.fresh_evaluations; break;
        case BenchmarkCounter::Incremental: ++g_benchmark_stats.incremental_evaluations; break;
        case BenchmarkCounter::Refresh: ++g_benchmark_stats.full_refreshes; break;
        case BenchmarkCounter::Inference: ++g_benchmark_stats.inferences; break;
        case BenchmarkCounter::CacheLookup: ++g_benchmark_stats.refresh_cache_lookups; break;
        case BenchmarkCounter::CacheHit: ++g_benchmark_stats.refresh_cache_hits; break;
        case BenchmarkCounter::CacheMiss: ++g_benchmark_stats.refresh_cache_misses; break;
        case BenchmarkCounter::CacheStore: ++g_benchmark_stats.refresh_cache_stores; break;
        case BenchmarkCounter::CacheReplacement: ++g_benchmark_stats.refresh_cache_replacements; break;
        case BenchmarkCounter::CacheInvalidation: ++g_benchmark_stats.refresh_cache_invalidations; break;
        case BenchmarkCounter::CacheUninitializedMiss: ++g_benchmark_stats.refresh_cache_uninitialized_misses; break;
        case BenchmarkCounter::CacheKeyMiss: ++g_benchmark_stats.refresh_cache_key_misses; break;
        case BenchmarkCounter::KingRefresh: ++g_benchmark_stats.king_bucket_refreshes; break;
    }
}

enum class DeltaMoveType : std::size_t {
    Normal, Capture, EnPassant, Promotion, Castling, KingMove, Null
};

DeltaMoveType classify_delta_move(const StateInfo::NnueDelta& delta) noexcept {
    if (delta.is_null) return DeltaMoveType::Null;
    if (delta.primary_piece == make_piece(Color::White, PieceType::King) ||
        delta.primary_piece == make_piece(Color::Black, PieceType::King)) {
        return delta.removed_piece != Piece::None && delta.added_piece != Piece::None
            ? DeltaMoveType::Castling : DeltaMoveType::KingMove;
    }
    if (delta.added_piece != Piece::None) return DeltaMoveType::Promotion;
    if (delta.removed_piece != Piece::None) {
        return delta.removed_square != delta.primary_to ? DeltaMoveType::EnPassant
                                                        : DeltaMoveType::Capture;
    }
    return DeltaMoveType::Normal;
}

[[maybe_unused]] void record_delta_histogram(const StateInfo::NnueDelta& delta,
                            std::size_t halfka_removed,
                            std::size_t halfka_added,
                            std::size_t threats_removed,
                            std::size_t threats_added,
                            std::size_t accumulator_passes) noexcept {
    constexpr std::uint64_t kAccumulatorBytes = kDimensions * sizeof(std::int16_t);
    const std::size_t dirty_rows = halfka_removed + halfka_added + threats_removed + threats_added;
    ++g_benchmark_stats.halfka_removed_features[std::min(halfka_removed,
        g_benchmark_stats.halfka_removed_features.size() - 1)];
    ++g_benchmark_stats.halfka_added_features[std::min(halfka_added,
        g_benchmark_stats.halfka_added_features.size() - 1)];
    ++g_benchmark_stats.full_threats_removed_features[std::min(threats_removed,
        g_benchmark_stats.full_threats_removed_features.size() - 1)];
    ++g_benchmark_stats.full_threats_added_features[std::min(threats_added,
        g_benchmark_stats.full_threats_added_features.size() - 1)];
    ++g_benchmark_stats.total_dirty_rows[std::min(dirty_rows,
        g_benchmark_stats.total_dirty_rows.size() - 1)];
    ++g_benchmark_stats.delta_move_types[static_cast<std::size_t>(classify_delta_move(delta))];
    g_benchmark_stats.accumulator_full_passes.fetch_add(accumulator_passes, std::memory_order_relaxed);
    g_benchmark_stats.accumulator_bytes_read.fetch_add(accumulator_passes * kAccumulatorBytes,
                                                        std::memory_order_relaxed);
    g_benchmark_stats.accumulator_bytes_written.fetch_add(accumulator_passes * kAccumulatorBytes,
                                                           std::memory_order_relaxed);
}
#else
class ProfileScope final {
public:
    explicit ProfileScope(NnueProfileComponent) noexcept {}
};
void record_kernel_call(bool) noexcept {}
enum class BenchmarkCounter {
    Fresh, Incremental, Refresh, Inference, CacheLookup, CacheHit, CacheMiss,
    CacheStore, CacheReplacement, CacheInvalidation, CacheUninitializedMiss, CacheKeyMiss, KingRefresh
};
void record_benchmark_counter(BenchmarkCounter) noexcept {}
#endif

template <typename Atomic>
void record(Atomic& counter) {
#if !defined(NDEBUG) || defined(BLAZE_NNUE_INSTRUMENTATION)
    counter.fetch_add(1, std::memory_order_relaxed);
#else
    static_cast<void>(counter);
#endif
}

Stockfish::Color to_sf_color(Color color) {
    return color == Color::White ? Stockfish::WHITE : Stockfish::BLACK;
}

Stockfish::Square to_sf_square(Square square) {
    return static_cast<Stockfish::Square>(square_index(square));
}

Stockfish::Piece to_sf_piece(Piece piece) {
    if (piece == Piece::None) return Stockfish::NO_PIECE;
    const int type = static_cast<int>(type_of(piece));
    return static_cast<Stockfish::Piece>(
        type + (color_of(piece) == Color::White ? 0 : 8));
}

struct DirectWeights {
    NetworkBig network;
    const nnue::KernelSet kernels;

    explicit DirectWeights(std::string_view path, bool allow_avx2) :
        network(Stockfish::Eval::NNUE::EvalFile{
                    Stockfish::FixedString<256>(""),
                    Stockfish::FixedString<256>(""),
                    Stockfish::FixedString<256>("")},
                Stockfish::Eval::NNUE::EmbeddedNNUEType::BIG),
        kernels(nnue::select_kernels(allow_avx2)) {
        network.load("", std::string(path));
        if (!network.is_loaded()) {
            throw std::runtime_error("could not load the requested Big NNUE network");
        }
        record(g_runtime_stats.network_loads);
    }
};

struct alignas(64) Accumulator {
    std::array<std::array<std::int16_t, kDimensions>, 2> pieces{};
    std::array<std::array<std::int16_t, kDimensions>, 2> threats{};
    std::array<std::array<std::int32_t, kBuckets>, 2> piece_psqt{};
    std::array<std::array<std::int32_t, kBuckets>, 2> threat_psqt{};
};

Square king_square(const Position& position, Color color) {
    const Bitboard kings = position.pieces(color, PieceType::King);
    return kings == 0 ? Square::None : static_cast<Square>(std::countr_zero(kings));
}

Bitboard attacks_from(const Position& position, Piece piece, Square from) {
    switch (type_of(piece)) {
        case PieceType::Pawn: return Attacks::pawn(color_of(piece), from);
        case PieceType::Knight: return Attacks::knight(from);
        case PieceType::Bishop: return Attacks::bishop(from, position.occupied());
        case PieceType::Rook: return Attacks::rook(from, position.occupied());
        case PieceType::Queen: return Attacks::queen(from, position.occupied());
        case PieceType::King: return Attacks::king(from);
        case PieceType::None: return 0;
    }
    return 0;
}

void apply_piece_feature(
    Accumulator& accumulator,
    const BigFeatureTransformer& transformer,
    Color perspective,
    Square king,
    Piece piece,
    Square square,
    int sign,
    const nnue::KernelSet& kernels) {
    const auto index = Stockfish::Eval::NNUE::Features::HalfKAv2_hm::make_index(
        to_sf_color(perspective), to_sf_square(square), to_sf_piece(piece), to_sf_square(king));
    const std::size_t side = static_cast<std::size_t>(perspective);
    const std::int16_t* const feature = transformer.weights.data() + index * kDimensions;
    record_kernel_call(kernels.avx2);
    (sign > 0 ? kernels.add : kernels.subtract)(accumulator.pieces[side].data(), feature);
    for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
        accumulator.piece_psqt[side][bucket] +=
            sign * transformer.psqtWeights[index * kBuckets + bucket];
    }
}

void apply_threat_feature(
    Accumulator& accumulator,
    const BigFeatureTransformer& transformer,
    Color perspective,
    Square king,
    const StateInfo::NnueThreatChange& threat,
    int sign) {
    const auto index = Stockfish::Eval::NNUE::Features::FullThreats::make_index(
        to_sf_color(perspective), to_sf_piece(threat.attacker), to_sf_square(threat.from),
        to_sf_square(threat.to), to_sf_piece(threat.attacked), to_sf_square(king));
    if (index >= Stockfish::Eval::NNUE::Features::FullThreats::Dimensions) return;
    const std::size_t side = static_cast<std::size_t>(perspective);
    for (std::size_t i = 0; i < kDimensions; ++i) {
        accumulator.threats[side][i] = static_cast<std::int16_t>(
            accumulator.threats[side][i] + sign * transformer.threatWeights[index * kDimensions + i]);
    }
    for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
        accumulator.threat_psqt[side][bucket] +=
            sign * transformer.threatPsqtWeights[index * kBuckets + bucket];
    }
}

const std::int16_t* piece_feature_row(const BigFeatureTransformer& transformer,
                                      Color perspective,
                                      Square king,
                                      Piece piece,
                                      Square square) {
    const auto index = Stockfish::Eval::NNUE::Features::HalfKAv2_hm::make_index(
        to_sf_color(perspective), to_sf_square(square), to_sf_piece(piece), to_sf_square(king));
    return transformer.weights.data() + index * kDimensions;
}

void apply_piece_psqt(Accumulator& accumulator,
                      const BigFeatureTransformer& transformer,
                      Color perspective,
                      Square king,
                      Piece piece,
                      Square square,
                      int sign) {
    const auto index = Stockfish::Eval::NNUE::Features::HalfKAv2_hm::make_index(
        to_sf_color(perspective), to_sf_square(square), to_sf_piece(piece), to_sf_square(king));
    const std::size_t side = static_cast<std::size_t>(perspective);
    for (std::size_t bucket = 0; bucket < kBuckets; ++bucket)
        accumulator.piece_psqt[side][bucket] += sign * transformer.psqtWeights[index * kBuckets + bucket];
}

const std::int8_t* threat_feature_row(const BigFeatureTransformer& transformer,
                                     Color perspective,
                                     Square king,
                                     const StateInfo::NnueThreatChange& threat) {
    const auto index = Stockfish::Eval::NNUE::Features::FullThreats::make_index(
        to_sf_color(perspective), to_sf_piece(threat.attacker), to_sf_square(threat.from),
        to_sf_square(threat.to), to_sf_piece(threat.attacked), to_sf_square(king));
    if (index >= Stockfish::Eval::NNUE::Features::FullThreats::Dimensions) return nullptr;
    return transformer.threatWeights.data() + index * kDimensions;
}

void apply_threat_psqt(Accumulator& accumulator,
                       const BigFeatureTransformer& transformer,
                       Color perspective,
                       Square king,
                       const StateInfo::NnueThreatChange& threat,
                       int sign) {
    const auto index = Stockfish::Eval::NNUE::Features::FullThreats::make_index(
        to_sf_color(perspective), to_sf_piece(threat.attacker), to_sf_square(threat.from),
        to_sf_square(threat.to), to_sf_piece(threat.attacked), to_sf_square(king));
    if (index >= Stockfish::Eval::NNUE::Features::FullThreats::Dimensions) return;
    const std::size_t side = static_cast<std::size_t>(perspective);
    for (std::size_t bucket = 0; bucket < kBuckets; ++bucket)
        accumulator.threat_psqt[side][bucket] += sign * transformer.threatPsqtWeights[index * kBuckets + bucket];
}

void apply_fused_rows(std::int16_t* accumulator,
                      const nnue::KernelSet& kernels,
                      const std::int16_t* const* removed,
                      std::size_t removed_count,
                      const std::int16_t* const* added,
                      std::size_t added_count) {
    if (removed_count == 0 && added_count == 0) return;
    for (std::size_t row = 0; row < removed_count + added_count; ++row)
        record_kernel_call(kernels.avx2);
    // The selected function pointer is outside the 1024-wide loop. The three
    // unrolled distributions are the measured normal, capture, and castling paths.
    const nnue::FusedAccumulateKernel kernel = removed_count == 1 && added_count == 1
        ? kernels.fused_1_1
        : removed_count == 2 && added_count == 1
        ? kernels.fused_2_1
        : removed_count == 2 && added_count == 2
        ? kernels.fused_2_2
        : kernels.fused;
    kernel(accumulator, removed, removed_count, added, added_count);
}

void apply_fused_threat_rows(std::int16_t* accumulator,
                             const nnue::KernelSet& kernels,
                             const std::int8_t* const* removed,
                             std::size_t removed_count,
                             const std::int8_t* const* added,
                             std::size_t added_count) {
    if (removed_count == 0 && added_count == 0) return;
    for (std::size_t row = 0; row < removed_count + added_count; ++row)
        record_kernel_call(kernels.avx2);
    const nnue::FusedThreatAccumulateKernel kernel = removed_count == 2 && added_count == 2
        ? kernels.fused_threats_2_2 : kernels.fused_threats;
    kernel(accumulator, removed, removed_count, added, added_count);
}

void refresh_perspective(Accumulator& accumulator,
                         const DirectWeights& weights,
                         const Position& position,
                         Color perspective,
                         bool refresh_halfka,
                         bool refresh_threats) {
    const auto& transformer = weights.network.transformer();
    const std::size_t side = static_cast<std::size_t>(perspective);
    const Square king = king_square(position, perspective);
    if (refresh_halfka) {
        accumulator.pieces[side] = transformer.biases;
        accumulator.piece_psqt[side].fill(0);
        ProfileScope timer(NnueProfileComponent::HalfKaRefresh);
        for (int sq = 0; sq < 64; ++sq) {
            const Square square = static_cast<Square>(sq);
            const Piece piece = position.piece_on(square);
            if (piece != Piece::None)
                apply_piece_feature(accumulator, transformer, perspective, king, piece, square, 1,
                                    weights.kernels);
        }
    }
    if (refresh_threats) {
        accumulator.threats[side].fill(0);
        accumulator.threat_psqt[side].fill(0);
        ProfileScope timer(NnueProfileComponent::FullThreatsRefresh);
        for (int sq = 0; sq < 64; ++sq) {
            const Square from = static_cast<Square>(sq);
            const Piece attacker = position.piece_on(from);
            Bitboard targets = attacker == Piece::None ? 0
                : attacks_from(position, attacker, from) & position.occupied();
            while (targets != 0) {
                const Square to = static_cast<Square>(std::countr_zero(targets));
                targets &= targets - 1;
                apply_threat_feature(accumulator, transformer, perspective, king,
                    StateInfo::NnueThreatChange{attacker, position.piece_on(to), from, to, true}, 1);
            }
        }
    }
}

void refresh(Accumulator& accumulator, const DirectWeights& weights, const Position& position) {
    record(g_runtime_stats.accumulator_refreshes);
    record_benchmark_counter(BenchmarkCounter::Refresh);
    refresh_perspective(accumulator, weights, position, Color::White, true, true);
    refresh_perspective(accumulator, weights, position, Color::Black, true, true);
}

int full_threats_orientation(Color perspective, Square king) {
    return Stockfish::Eval::NNUE::Features::FullThreats::OrientTBL[to_sf_square(king)] ^
           (56 * static_cast<int>(to_sf_color(perspective)));
}

void apply_delta(
    Accumulator& accumulator,
    const DirectWeights& weights,
    const Position& position_after,
    const StateInfo::NnueDelta& delta) {
    if (delta.is_null) return;
    record(g_runtime_stats.incremental_updates);
    const auto& transformer = weights.network.transformer();
    const bool king_move = delta.primary_piece == make_piece(Color::White, PieceType::King) ||
                           delta.primary_piece == make_piece(Color::Black, PieceType::King);
#if defined(BLAZE_NNUE_BENCHMARK)
    std::size_t halfka_removed = 0;
    std::size_t halfka_added = 0;
    std::size_t threats_removed = 0;
    std::size_t threats_added = 0;
    std::size_t accumulator_passes = 0;
#endif
    for (const Color perspective : {Color::White, Color::Black}) {
        const Square king = king_square(position_after, perspective);
        const bool halfka_stale = king_move && perspective == delta.moving_side;
        const Square king_before = perspective == Color::White
            ? delta.white_king_before : delta.black_king_before;
        // FullThreats indexes only the orientation class of the perspective
        // king. A king move inside that class remains an ordinary dirty threat
        // update; crossing classes needs a refresh for this perspective only.
        const bool threats_stale = halfka_stale &&
            full_threats_orientation(perspective, king_before) !=
            full_threats_orientation(perspective, king);
        if (halfka_stale) record_benchmark_counter(BenchmarkCounter::KingRefresh);
        if (halfka_stale || threats_stale)
            refresh_perspective(accumulator, weights, position_after, perspective,
                                halfka_stale, threats_stale);
        if (!halfka_stale) {
            ProfileScope timer(NnueProfileComponent::HalfKaIncremental);
            std::array<const std::int16_t*, 2> removed{};
            std::array<const std::int16_t*, 2> added{};
            std::size_t removed_count = 0;
            std::size_t added_count = 0;
            removed[removed_count++] = piece_feature_row(transformer, perspective, king,
                delta.primary_piece, delta.primary_from);
            apply_piece_psqt(accumulator, transformer, perspective, king,
                delta.primary_piece, delta.primary_from, -1);
#if defined(BLAZE_NNUE_BENCHMARK)
            ++halfka_removed;
#endif
            if (delta.primary_to != Square::None) {
                added[added_count++] = piece_feature_row(transformer, perspective, king,
                    delta.primary_piece, delta.primary_to);
                apply_piece_psqt(accumulator, transformer, perspective, king,
                    delta.primary_piece, delta.primary_to, 1);
#if defined(BLAZE_NNUE_BENCHMARK)
                ++halfka_added;
#endif
            }
            if (delta.removed_piece != Piece::None) {
                removed[removed_count++] = piece_feature_row(transformer, perspective, king,
                    delta.removed_piece, delta.removed_square);
                apply_piece_psqt(accumulator, transformer, perspective, king,
                    delta.removed_piece, delta.removed_square, -1);
#if defined(BLAZE_NNUE_BENCHMARK)
                ++halfka_removed;
#endif
            }
            if (delta.added_piece != Piece::None) {
                added[added_count++] = piece_feature_row(transformer, perspective, king,
                    delta.added_piece, delta.added_square);
                apply_piece_psqt(accumulator, transformer, perspective, king,
                    delta.added_piece, delta.added_square, 1);
#if defined(BLAZE_NNUE_BENCHMARK)
                ++halfka_added;
#endif
            }
            apply_fused_rows(accumulator.pieces[static_cast<std::size_t>(perspective)].data(),
                             weights.kernels, removed.data(), removed_count, added.data(), added_count);
#if defined(BLAZE_NNUE_BENCHMARK)
            ++accumulator_passes;
#endif
        }
        if (!threats_stale) {
            ProfileScope timer(NnueProfileComponent::FullThreatsIncremental);
            std::array<const std::int8_t*, StateInfo::NnueDelta::max_threat_changes> removed{};
            std::array<const std::int8_t*, StateInfo::NnueDelta::max_threat_changes> added{};
            std::size_t removed_count = 0;
            std::size_t added_count = 0;
            for (std::size_t i = 0; i < delta.threat_count; ++i) {
                const auto* const row = threat_feature_row(transformer, perspective, king, delta.threats[i]);
#if defined(BLAZE_NNUE_BENCHMARK)
                if (row != nullptr) {
                    if (delta.threats[i].added) ++threats_added;
                    else ++threats_removed;
                }
#endif
                if (row == nullptr) continue;
                apply_threat_psqt(accumulator, transformer, perspective, king, delta.threats[i],
                                  delta.threats[i].added ? 1 : -1);
                if (delta.threats[i].added) added[added_count++] = row;
                else removed[removed_count++] = row;
            }
            if (removed_count != 0 || added_count != 0) {
                apply_fused_threat_rows(accumulator.threats[static_cast<std::size_t>(perspective)].data(),
                                        weights.kernels, removed.data(), removed_count, added.data(), added_count);
#if defined(BLAZE_NNUE_BENCHMARK)
                ++accumulator_passes;
#endif
            }
        }
    }
#if defined(BLAZE_NNUE_BENCHMARK)
    record_delta_histogram(delta, halfka_removed, halfka_added, threats_removed, threats_added,
                           accumulator_passes);
#endif
}

struct RawOutput {
    std::int32_t psqt = 0;
    std::int32_t positional = 0;
    int raw = 0;
};

RawOutput propagate(
    const Accumulator& accumulator,
    const DirectWeights& weights,
    const Position& position,
    std::array<std::uint8_t, kDimensions>& transformed,
    nnue::InferenceScratch& scratch) {
    const std::size_t us = static_cast<std::size_t>(position.side_to_move());
    const std::size_t them = static_cast<std::size_t>(opposite(position.side_to_move()));
    const std::size_t bucket = (std::popcount(position.occupied()) - 1) / 4;
    const std::array<std::size_t, 2> perspectives{us, them};
    for (std::size_t p = 0; p < 2; ++p) {
        const std::size_t offset = p * (kDimensions / 2);
        const std::size_t side = perspectives[p];
        ProfileScope timer(NnueProfileComponent::FeatureTransform);
        record_kernel_call(weights.kernels.avx2);
        weights.kernels.transform(accumulator.pieces[side].data(), accumulator.threats[side].data(),
                                  transformed.data() + offset);
    }
    RawOutput output;
    {
        ProfileScope timer(NnueProfileComponent::Psqt);
        output.psqt =
            (accumulator.piece_psqt[us][bucket] - accumulator.piece_psqt[them][bucket] +
             accumulator.threat_psqt[us][bucket] - accumulator.threat_psqt[them][bucket]) / 2;
    }
    const auto& architecture = weights.network.architecture(bucket);
    // The kernel emits its affine/activation subcomponent timings in the
    // benchmark build; this call is still one dispatch-free function pointer.
    record_kernel_call(weights.kernels.avx2);
    output.positional = weights.kernels.propagate(
        transformed.data(),
        nnue::InferenceWeights{
            architecture.fc_0.biases_data(), architecture.fc_0.weights_data(),
            architecture.fc_1.biases_data(), architecture.fc_1.weights_data(),
            architecture.fc_2.biases_data(), architecture.fc_2.weights_data()},
        scratch);
    // The legacy bridge exposed the two Network::evaluate() components after
    // their internal OutputScale division, then divided their sum once more.
    // Keep that public score contract while the bridge remains the oracle.
    // Match Network::evaluate(): each component is converted from network
    // units independently before its wrapper combines the two values.
    output.raw = static_cast<int>(output.psqt / Stockfish::Eval::NNUE::OutputScale) +
                 static_cast<int>(output.positional / Stockfish::Eval::NNUE::OutputScale);
    return output;
}

NnueDebugSnapshot make_snapshot(
    const Accumulator& accumulator,
    const DirectWeights& weights,
    const Position& position) {
    NnueDebugSnapshot snapshot;
    snapshot.halfka = accumulator.pieces;
    snapshot.threats = accumulator.threats;
    snapshot.halfka_psqt = accumulator.piece_psqt;
    snapshot.threat_psqt = accumulator.threat_psqt;
    nnue::InferenceScratch scratch;
    const RawOutput output = propagate(accumulator, weights, position, snapshot.transformed, scratch);
    snapshot.psqt_output = output.psqt;
    snapshot.positional_output = output.positional;
    snapshot.raw_output = output.raw;
    return snapshot;
}

}  // namespace

void reset_nnue_runtime_stats() {
    g_runtime_stats.network_loads.store(0, std::memory_order_relaxed);
    g_runtime_stats.thread_state_constructions.store(0, std::memory_order_relaxed);
    g_runtime_stats.root_tasks.store(0, std::memory_order_relaxed);
    g_runtime_stats.root_task_state_constructions.store(0, std::memory_order_relaxed);
    g_runtime_stats.hot_path_heap_allocations.store(0, std::memory_order_relaxed);
    g_runtime_stats.fen_serializations.store(0, std::memory_order_relaxed);
    g_runtime_stats.stockfish_position_constructions.store(0, std::memory_order_relaxed);
    g_runtime_stats.accumulator_refreshes.store(0, std::memory_order_relaxed);
    g_runtime_stats.refresh_cache_hits.store(0, std::memory_order_relaxed);
    g_runtime_stats.incremental_updates.store(0, std::memory_order_relaxed);
    g_runtime_stats.evaluations.store(0, std::memory_order_relaxed);
}

NnueRuntimeStats nnue_runtime_stats() {
    return NnueRuntimeStats{
        g_runtime_stats.network_loads.load(std::memory_order_relaxed),
        g_runtime_stats.thread_state_constructions.load(std::memory_order_relaxed),
        g_runtime_stats.root_tasks.load(std::memory_order_relaxed),
        g_runtime_stats.root_task_state_constructions.load(std::memory_order_relaxed),
        g_runtime_stats.hot_path_heap_allocations.load(std::memory_order_relaxed),
        g_runtime_stats.fen_serializations.load(std::memory_order_relaxed),
        g_runtime_stats.stockfish_position_constructions.load(std::memory_order_relaxed),
        g_runtime_stats.accumulator_refreshes.load(std::memory_order_relaxed),
        g_runtime_stats.refresh_cache_hits.load(std::memory_order_relaxed),
        g_runtime_stats.incremental_updates.load(std::memory_order_relaxed),
        g_runtime_stats.evaluations.load(std::memory_order_relaxed)};
}

void reset_nnue_benchmark_stats() {
#if defined(BLAZE_NNUE_BENCHMARK) || !defined(NDEBUG)
    g_benchmark_stats.scalar_kernel_calls.store(0, std::memory_order_relaxed);
    g_benchmark_stats.avx2_kernel_calls.store(0, std::memory_order_relaxed);
    g_benchmark_stats.fresh_evaluations.store(0, std::memory_order_relaxed);
    g_benchmark_stats.incremental_evaluations.store(0, std::memory_order_relaxed);
    g_benchmark_stats.full_refreshes.store(0, std::memory_order_relaxed);
    g_benchmark_stats.inferences.store(0, std::memory_order_relaxed);
    g_benchmark_stats.refresh_cache_lookups.store(0, std::memory_order_relaxed);
    g_benchmark_stats.refresh_cache_hits.store(0, std::memory_order_relaxed);
    g_benchmark_stats.refresh_cache_misses.store(0, std::memory_order_relaxed);
    g_benchmark_stats.refresh_cache_stores.store(0, std::memory_order_relaxed);
    g_benchmark_stats.refresh_cache_replacements.store(0, std::memory_order_relaxed);
    g_benchmark_stats.refresh_cache_invalidations.store(0, std::memory_order_relaxed);
    g_benchmark_stats.refresh_cache_uninitialized_misses.store(0, std::memory_order_relaxed);
    g_benchmark_stats.refresh_cache_key_misses.store(0, std::memory_order_relaxed);
    g_benchmark_stats.refresh_cache_hit_bytes.store(0, std::memory_order_relaxed);
    for (auto& perspective : g_benchmark_stats.refresh_cache_hits_by_perspective_bucket)
        for (auto& bucket : perspective) bucket.store(0, std::memory_order_relaxed);
    g_benchmark_stats.king_bucket_refreshes.store(0, std::memory_order_relaxed);
    for (auto& value : g_benchmark_stats.halfka_removed_features) value.store(0, std::memory_order_relaxed);
    for (auto& value : g_benchmark_stats.halfka_added_features) value.store(0, std::memory_order_relaxed);
    for (auto& value : g_benchmark_stats.full_threats_removed_features) value.store(0, std::memory_order_relaxed);
    for (auto& value : g_benchmark_stats.full_threats_added_features) value.store(0, std::memory_order_relaxed);
    for (auto& value : g_benchmark_stats.total_dirty_rows) value.store(0, std::memory_order_relaxed);
    for (auto& value : g_benchmark_stats.delta_move_types) value.store(0, std::memory_order_relaxed);
    g_benchmark_stats.accumulator_full_passes.store(0, std::memory_order_relaxed);
    g_benchmark_stats.accumulator_bytes_read.store(0, std::memory_order_relaxed);
    g_benchmark_stats.accumulator_bytes_written.store(0, std::memory_order_relaxed);
    for (AtomicProfileComponent& component : g_benchmark_stats.components) {
        component.calls.store(0, std::memory_order_relaxed);
        component.sampled_calls.store(0, std::memory_order_relaxed);
        component.sampled_nanoseconds.store(0, std::memory_order_relaxed);
    }
#endif
}

NnueBenchmarkStats nnue_benchmark_stats() {
    NnueBenchmarkStats result;
    result.avx2_supported = nnue_avx2_supported();
#if defined(BLAZE_NNUE_BENCHMARK) || !defined(NDEBUG)
    result.scalar_kernel_calls = g_benchmark_stats.scalar_kernel_calls.load(std::memory_order_relaxed);
    result.avx2_kernel_calls = g_benchmark_stats.avx2_kernel_calls.load(std::memory_order_relaxed);
    result.fresh_evaluations = g_benchmark_stats.fresh_evaluations.load(std::memory_order_relaxed);
    result.incremental_evaluations = g_benchmark_stats.incremental_evaluations.load(std::memory_order_relaxed);
    result.full_refreshes = g_benchmark_stats.full_refreshes.load(std::memory_order_relaxed);
    result.inferences = g_benchmark_stats.inferences.load(std::memory_order_relaxed);
    result.refresh_cache_lookups = g_benchmark_stats.refresh_cache_lookups.load(std::memory_order_relaxed);
    result.refresh_cache_hits = g_benchmark_stats.refresh_cache_hits.load(std::memory_order_relaxed);
    result.refresh_cache_misses = g_benchmark_stats.refresh_cache_misses.load(std::memory_order_relaxed);
    result.refresh_cache_stores = g_benchmark_stats.refresh_cache_stores.load(std::memory_order_relaxed);
    result.refresh_cache_replacements = g_benchmark_stats.refresh_cache_replacements.load(std::memory_order_relaxed);
    result.refresh_cache_invalidations = g_benchmark_stats.refresh_cache_invalidations.load(std::memory_order_relaxed);
    result.refresh_cache_uninitialized_misses = g_benchmark_stats.refresh_cache_uninitialized_misses.load(std::memory_order_relaxed);
    result.refresh_cache_key_misses = g_benchmark_stats.refresh_cache_key_misses.load(std::memory_order_relaxed);
    result.refresh_cache_hit_bytes = g_benchmark_stats.refresh_cache_hit_bytes.load(std::memory_order_relaxed);
    for (std::size_t perspective = 0; perspective < 2; ++perspective)
        for (std::size_t bucket = 0; bucket < 8; ++bucket)
            result.refresh_cache_hits_by_perspective_bucket[perspective][bucket] =
                g_benchmark_stats.refresh_cache_hits_by_perspective_bucket[perspective][bucket].load(
                    std::memory_order_relaxed);
    result.king_bucket_refreshes = g_benchmark_stats.king_bucket_refreshes.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.halfka_removed_features.size(); ++i)
        result.halfka_removed_features[i] = g_benchmark_stats.halfka_removed_features[i].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.halfka_added_features.size(); ++i)
        result.halfka_added_features[i] = g_benchmark_stats.halfka_added_features[i].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.full_threats_removed_features.size(); ++i)
        result.full_threats_removed_features[i] = g_benchmark_stats.full_threats_removed_features[i].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.full_threats_added_features.size(); ++i)
        result.full_threats_added_features[i] = g_benchmark_stats.full_threats_added_features[i].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.total_dirty_rows.size(); ++i)
        result.total_dirty_rows[i] = g_benchmark_stats.total_dirty_rows[i].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.delta_move_types.size(); ++i)
        result.delta_move_types[i] = g_benchmark_stats.delta_move_types[i].load(std::memory_order_relaxed);
    result.accumulator_full_passes = g_benchmark_stats.accumulator_full_passes.load(std::memory_order_relaxed);
    result.accumulator_bytes_read = g_benchmark_stats.accumulator_bytes_read.load(std::memory_order_relaxed);
    result.accumulator_bytes_written = g_benchmark_stats.accumulator_bytes_written.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < result.components.size(); ++i) {
        const AtomicProfileComponent& source = g_benchmark_stats.components[i];
        result.components[i] = NnueProfileComponentStats{
            source.calls.load(std::memory_order_relaxed),
            source.sampled_calls.load(std::memory_order_relaxed),
            source.sampled_nanoseconds.load(std::memory_order_relaxed)};
    }
#endif
    return result;
}

bool nnue_avx2_supported() noexcept {
#if (defined(__i386__) || defined(__x86_64__)) && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

bool nnue_benchmark_should_sample_delta() {
#if defined(BLAZE_NNUE_BENCHMARK) || !defined(NDEBUG)
    return profile_component_should_sample(NnueProfileComponent::DeltaConstruction);
#else
    return false;
#endif
}

void nnue_benchmark_record_delta_construction(std::uint64_t nanoseconds) {
#if defined(BLAZE_NNUE_BENCHMARK) || !defined(NDEBUG)
    profile_component_sample(NnueProfileComponent::DeltaConstruction, nanoseconds);
#else
    static_cast<void>(nanoseconds);
#endif
}

bool nnue_benchmark_should_sample_component(NnueProfileComponent component) {
#if defined(BLAZE_NNUE_BENCHMARK) || !defined(NDEBUG)
    return profile_component_should_sample(component);
#else
    static_cast<void>(component);
    return false;
#endif
}

void nnue_benchmark_record_component_sample(NnueProfileComponent component,
                                            std::uint64_t nanoseconds) {
#if defined(BLAZE_NNUE_BENCHMARK) || !defined(NDEBUG)
    profile_component_sample(component, nanoseconds);
#else
    static_cast<void>(component);
    static_cast<void>(nanoseconds);
#endif
}

const char* nnue_profile_component_name(NnueProfileComponent component) noexcept {
    constexpr std::array names{
        "nnue_delta", "halfka_incremental", "halfka_refresh", "full_threats_incremental",
        "full_threats_refresh", "refresh_cache_lookup", "feature_transform", "psqt",
        "first_affine", "hidden_affine", "activation", "output_layer", "public_score"};
    const std::size_t index = static_cast<std::size_t>(component);
    return index < names.size() ? names[index] : "unknown";
}

void note_legacy_nnue_bridge_evaluation() {
    record(g_runtime_stats.fen_serializations);
    record(g_runtime_stats.stockfish_position_constructions);
}

NnueRootTaskScope::NnueRootTaskScope() {
#if !defined(NDEBUG) || defined(BLAZE_NNUE_INSTRUMENTATION)
    ++g_root_task_depth;
    record(g_runtime_stats.root_tasks);
#endif
}

NnueRootTaskScope::~NnueRootTaskScope() {
#if !defined(NDEBUG) || defined(BLAZE_NNUE_INSTRUMENTATION)
    --g_root_task_depth;
#endif
}

struct NetworkEvaluator::Impl {
    explicit Impl(std::string_view path, bool allow_avx2) :
        weights(std::make_shared<DirectWeights>(path, allow_avx2)) {}
    std::shared_ptr<const DirectWeights> weights;
};

struct NnueThreadState::Impl {
    explicit Impl(std::shared_ptr<const DirectWeights> network) : weights(std::move(network)) {}
    std::shared_ptr<const DirectWeights> weights;
    std::array<Accumulator, kPlyCapacity> stack{};
    std::uint64_t refresh_cache_key = 0;
    bool refresh_cache_valid = false;
    std::size_t ply = 0;
    bool fresh_evaluation = false;
    alignas(64) mutable std::array<std::uint8_t, kDimensions> inference_scratch{};
    mutable nnue::InferenceScratch inference_buffers{};
};

std::optional<NetworkEvaluator> NetworkEvaluator::create(std::string_view path, std::string& error) {
    error.clear();
    try {
        NetworkEvaluator evaluator;
        evaluator.impl_ = std::make_unique<Impl>(path, true);
        return evaluator;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

std::optional<NetworkEvaluator> NetworkEvaluator::create_scalar_oracle(
    std::string_view path,
    std::string& error) {
    error.clear();
    try {
        NetworkEvaluator evaluator;
        evaluator.impl_ = std::make_unique<Impl>(path, false);
        return evaluator;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

NetworkEvaluator::NetworkEvaluator(NetworkEvaluator&& other) noexcept = default;
NetworkEvaluator& NetworkEvaluator::operator=(NetworkEvaluator&& other) noexcept = default;
NetworkEvaluator::~NetworkEvaluator() = default;

NnueThreadState::NnueThreadState(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NnueThreadState::NnueThreadState(NnueThreadState&& other) noexcept = default;
NnueThreadState& NnueThreadState::operator=(NnueThreadState&& other) noexcept = default;
NnueThreadState::~NnueThreadState() = default;

NnueThreadState NetworkEvaluator::make_thread_state() const {
    record(g_runtime_stats.thread_state_constructions);
    if (g_root_task_depth != 0) {
        record(g_runtime_stats.root_task_state_constructions);
        record(g_runtime_stats.hot_path_heap_allocations);
    }
    return NnueThreadState{std::make_unique<NnueThreadState::Impl>(impl_->weights)};
}

void NnueThreadState::reset(const Position& position) {
    impl_->ply = 0;
    ProfileScope timer(NnueProfileComponent::RefreshCacheLookup);
    record_benchmark_counter(BenchmarkCounter::CacheLookup);
    if (impl_->refresh_cache_valid && impl_->refresh_cache_key == position.key()) {
        record(g_runtime_stats.refresh_cache_hits);
        record_benchmark_counter(BenchmarkCounter::CacheHit);
        return;
    }
    record_benchmark_counter(BenchmarkCounter::CacheMiss);
    record_benchmark_counter(impl_->refresh_cache_valid
        ? BenchmarkCounter::CacheKeyMiss : BenchmarkCounter::CacheUninitializedMiss);
    if (impl_->refresh_cache_valid) record_benchmark_counter(BenchmarkCounter::CacheReplacement);
    refresh(impl_->stack[0], *impl_->weights, position);
    impl_->refresh_cache_key = position.key();
    impl_->refresh_cache_valid = true;
    record_benchmark_counter(BenchmarkCounter::CacheStore);
}

void NnueThreadState::push(const Position& position_after, const StateInfo& move_state) {
    if (impl_->ply + 1 >= impl_->stack.size()) return;
    impl_->stack[impl_->ply + 1] = impl_->stack[impl_->ply];
    ++impl_->ply;
    apply_delta(impl_->stack[impl_->ply], *impl_->weights, position_after, move_state.nnue);
}

void NnueThreadState::pop() {
    if (impl_->ply > 0) --impl_->ply;
}

int sf_nnue_public_score(int raw_network_output) {
    ProfileScope timer(NnueProfileComponent::PublicScore);
    return std::clamp(raw_network_output / Stockfish::Eval::NNUE::OutputScale,
        -search_mate_threshold + 1, search_mate_threshold - 1);
}

int NnueThreadState::raw_evaluate(const Position& position) const {
    record(g_runtime_stats.evaluations);
    record_benchmark_counter(impl_->fresh_evaluation
        ? BenchmarkCounter::Fresh : BenchmarkCounter::Incremental);
    record_benchmark_counter(BenchmarkCounter::Inference);
    return propagate(
        impl_->stack[impl_->ply],
        *impl_->weights,
        position,
        impl_->inference_scratch,
        impl_->inference_buffers).raw;
}

NnueDebugSnapshot NnueThreadState::debug_snapshot(const Position& position) const {
    return make_snapshot(impl_->stack[impl_->ply], *impl_->weights, position);
}

int NnueThreadState::evaluate(const Position& position) const {
    return sf_nnue_public_score(raw_evaluate(position));
}

int NetworkEvaluator::evaluate(const Position& position) const {
    return sf_nnue_public_score(raw_evaluate(position));
}

int NetworkEvaluator::raw_evaluate(const Position& position) const {
    auto state = make_thread_state();
    state.impl_->fresh_evaluation = true;
    state.reset(position);
    return state.raw_evaluate(position);
}

bool NetworkEvaluator::uses_avx2() const noexcept {
    return impl_ != nullptr && impl_->weights->kernels.avx2;
}

NnueDebugSnapshot NetworkEvaluator::debug_snapshot(const Position& position) const {
    auto state = make_thread_state();
    state.reset(position);
    return state.debug_snapshot(position);
}

}  // namespace blaze
