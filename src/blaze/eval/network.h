#ifndef BLAZE_EVAL_NETWORK_H
#define BLAZE_EVAL_NETWORK_H

#include "blaze/core/position.h"

#include <memory>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace blaze {

[[nodiscard]] int sf_nnue_public_score(int raw_network_output);

enum class NnueProfileComponent : std::uint8_t {
    DeltaConstruction,
    HalfKaIncremental,
    HalfKaRefresh,
    FullThreatsIncremental,
    FullThreatsRefresh,
    RefreshCacheLookup,
    FeatureTransform,
    Psqt,
    FirstAffine,
    HiddenAffine,
    Activation,
    OutputLayer,
    PublicScore,
    Count,
};

struct NnueProfileComponentStats {
    std::uint64_t calls = 0;
    std::uint64_t sampled_calls = 0;
    std::uint64_t sampled_nanoseconds = 0;
};

struct NnueBenchmarkStats {
    bool avx2_supported = false;
    std::uint64_t scalar_kernel_calls = 0;
    std::uint64_t avx2_kernel_calls = 0;
    std::uint64_t fresh_evaluations = 0;
    std::uint64_t incremental_evaluations = 0;
    std::uint64_t full_refreshes = 0;
    std::uint64_t inferences = 0;
    std::uint64_t refresh_cache_lookups = 0;
    std::uint64_t refresh_cache_hits = 0;
    std::uint64_t refresh_cache_misses = 0;
    std::uint64_t refresh_cache_stores = 0;
    std::uint64_t refresh_cache_replacements = 0;
    std::uint64_t refresh_cache_invalidations = 0;
    std::uint64_t refresh_cache_uninitialized_misses = 0;
    std::uint64_t refresh_cache_key_misses = 0;
    std::uint64_t refresh_cache_hit_bytes = 0;
    std::array<std::array<std::uint64_t, 8>, 2> refresh_cache_hits_by_perspective_bucket{};
    std::uint64_t king_bucket_refreshes = 0;
    // Per-move accumulator work distribution. These arrays are populated only
    // in the profile binary; their fixed bounds match NnueDelta capacity.
    std::array<std::uint64_t, 3> halfka_removed_features{};
    std::array<std::uint64_t, 3> halfka_added_features{};
    std::array<std::uint64_t, 97> full_threats_removed_features{};
    std::array<std::uint64_t, 97> full_threats_added_features{};
    std::array<std::uint64_t, 197> total_dirty_rows{};
    std::array<std::uint64_t, 7> delta_move_types{};
    std::uint64_t accumulator_full_passes = 0;
    std::uint64_t accumulator_bytes_read = 0;
    std::uint64_t accumulator_bytes_written = 0;
    std::array<NnueProfileComponentStats,
               static_cast<std::size_t>(NnueProfileComponent::Count)> components{};
};

struct NnueRuntimeStats {
    std::uint64_t network_loads = 0;
    std::uint64_t thread_state_constructions = 0;
    std::uint64_t root_tasks = 0;
    std::uint64_t root_task_state_constructions = 0;
    std::uint64_t hot_path_heap_allocations = 0;
    std::uint64_t fen_serializations = 0;
    std::uint64_t stockfish_position_constructions = 0;
    std::uint64_t accumulator_refreshes = 0;
    std::uint64_t refresh_cache_hits = 0;
    std::uint64_t incremental_updates = 0;
    std::uint64_t evaluations = 0;
};

void reset_nnue_runtime_stats();
[[nodiscard]] NnueRuntimeStats nnue_runtime_stats();
void note_legacy_nnue_bridge_evaluation();

// Profiling is compiled into test and benchmark binaries only. Normal release
// builds retain no counters or clock reads in NNUE hot paths.
void reset_nnue_benchmark_stats();
[[nodiscard]] NnueBenchmarkStats nnue_benchmark_stats();
[[nodiscard]] bool nnue_avx2_supported() noexcept;
void nnue_benchmark_record_delta_construction(std::uint64_t nanoseconds);
[[nodiscard]] bool nnue_benchmark_should_sample_delta();
[[nodiscard]] bool nnue_benchmark_should_sample_component(NnueProfileComponent component);
void nnue_benchmark_record_component_sample(NnueProfileComponent component,
                                            std::uint64_t nanoseconds);
[[nodiscard]] const char* nnue_profile_component_name(NnueProfileComponent component) noexcept;

class NnueRootTaskScope final {
public:
    NnueRootTaskScope();
    ~NnueRootTaskScope();

    NnueRootTaskScope(const NnueRootTaskScope&) = delete;
    NnueRootTaskScope& operator=(const NnueRootTaskScope&) = delete;
};

// Test-only exact oracle surface. It is a value copy and is never used by
// search or normal evaluation.
struct NnueDebugSnapshot {
    std::array<std::array<std::int16_t, 1024>, 2> halfka{};
    std::array<std::array<std::int16_t, 1024>, 2> threats{};
    std::array<std::array<std::int32_t, 8>, 2> halfka_psqt{};
    std::array<std::array<std::int32_t, 8>, 2> threat_psqt{};
    std::array<std::uint8_t, 1024> transformed{};
    std::int32_t psqt_output = 0;
    std::int32_t positional_output = 0;
    int raw_output = 0;
};

class NnueThreadState final {
public:
    NnueThreadState(NnueThreadState&&) noexcept;
    NnueThreadState& operator=(NnueThreadState&&) noexcept;
    ~NnueThreadState();

    NnueThreadState(const NnueThreadState&) = delete;
    NnueThreadState& operator=(const NnueThreadState&) = delete;

    void reset(const Position& position);
    void push(const Position& position_after, const StateInfo& move_state);
    void pop();
    [[nodiscard]] int evaluate(const Position& position) const;
    [[nodiscard]] int raw_evaluate(const Position& position) const;
    [[nodiscard]] NnueDebugSnapshot debug_snapshot(const Position& position) const;

private:
    struct Impl;
    explicit NnueThreadState(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
    friend class NetworkEvaluator;
};

class NetworkEvaluator final {
public:
    static std::optional<NetworkEvaluator> create(
        std::string_view path, std::string& error);
    // Test oracle only: retains scalar execution while production auto-dispatches.
    static std::optional<NetworkEvaluator> create_scalar_oracle(
        std::string_view path, std::string& error);

    NetworkEvaluator(NetworkEvaluator&& other) noexcept;
    NetworkEvaluator& operator=(NetworkEvaluator&& other) noexcept;
    ~NetworkEvaluator();

    [[nodiscard]] int evaluate(const Position& position) const;
    [[nodiscard]] int raw_evaluate(const Position& position) const;
    [[nodiscard]] NnueThreadState make_thread_state() const;
    [[nodiscard]] NnueDebugSnapshot debug_snapshot(const Position& position) const;
    [[nodiscard]] bool uses_avx2() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    NetworkEvaluator() = default;
};

}  // namespace blaze

#endif  // BLAZE_EVAL_NETWORK_H
