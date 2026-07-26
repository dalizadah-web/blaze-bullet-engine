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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    NetworkEvaluator() = default;
};

}  // namespace blaze

#endif  // BLAZE_EVAL_NETWORK_H
