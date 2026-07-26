#include "blaze/core/attacks.h"
#include "blaze/core/movegen.h"
#include "blaze/eval/network.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace blaze;
using Clock = std::chrono::steady_clock;

constexpr std::array<const char*, 8> kFens{
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/2NP4/PPP2PPP/R1BQK2R w KQkq - 4 5",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/5k2/3p4/1p1Pp2p/pP2Pp1P/P4P1K/8/8 b - - 0 1",
    "4k2r/6r1/8/8/8/8/3R4/R3K3 w Qk - 0 1",
    "r2q1rk1/pp2bppp/2n1pn2/2bp4/8/1P1P1NP1/PBPNPPBP/R2Q1RK1 w - - 5 9",
    "2r2rk1/pp1bqppp/2np1n2/8/2B1P3/1PN2N2/PBQ2PPP/2RR2K1 b - - 3 12",
    "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2"};

enum class Mode { Inference, Refresh, Full, Incremental };

const char* mode_name(Mode mode) {
    switch (mode) {
        case Mode::Inference: return "inference";
        case Mode::Refresh: return "fresh_refresh";
        case Mode::Full: return "full_direct";
        case Mode::Incremental: return "incremental";
    }
    return "unknown";
}

struct alignas(64) WorkerResult {
    std::uint64_t operations = 0;
    std::int64_t score_sum = 0;
};

std::uint64_t run_mode(const NetworkEvaluator& evaluator,
                       const std::array<Position, kFens.size()>& positions,
                       Mode mode,
                       unsigned threads,
                       std::chrono::milliseconds duration) {
    std::vector<WorkerResult> results(threads);
    std::barrier start(static_cast<std::ptrdiff_t>(threads + 1));
    const Clock::time_point deadline = Clock::now() + duration;
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (unsigned worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&, worker] {
            Position root = positions[worker % positions.size()];
            MoveList legal;
            generate_legal(root, legal);
            if (legal.empty()) return;
            const Move move = legal[worker % legal.size()];
            auto state = evaluator.make_thread_state();
            state.reset(root);
            WorkerResult local;
            start.arrive_and_wait();
            std::size_t index = worker;
            while (Clock::now() < deadline) {
                Position position = positions[index % positions.size()];
                switch (mode) {
                    case Mode::Inference:
                        local.score_sum += state.raw_evaluate(root);
                        break;
                    case Mode::Refresh:
                        state.reset(position);
                        break;
                    case Mode::Full:
                        state.reset(position);
                        local.score_sum += state.raw_evaluate(position);
                        break;
                    case Mode::Incremental: {
                        StateInfo move_state;
                        if (!root.make_move(move, move_state, true)) std::abort();
                        state.push(root, move_state);
                        local.score_sum += state.raw_evaluate(root);
                        state.pop();
                        root.unmake_move(move, move_state);
                        break;
                    }
                }
                ++local.operations;
                ++index;
            }
            results[worker] = local;
        });
    }
    start.arrive_and_wait();
    for (std::thread& worker : workers) worker.join();
    std::uint64_t total = 0;
    std::int64_t checksum = 0;
    for (const WorkerResult& result : results) {
        total += result.operations;
        checksum += result.score_sum;
    }
    std::cerr << "checksum=" << checksum << '\n';
    return total;
}

std::array<Position, kFens.size()> load_positions() {
    std::array<Position, kFens.size()> positions{};
    for (std::size_t i = 0; i < kFens.size(); ++i) {
        const auto position = Position::from_fen(kFens[i]);
        if (!position) std::abort();
        positions[i] = *position;
    }
    return positions;
}

void benchmark_evaluator(const char* label,
                         const NetworkEvaluator& evaluator,
                         const std::array<Position, kFens.size()>& positions,
                         std::chrono::milliseconds duration) {
    const std::array<unsigned, 4> thread_counts{1, 2, 4, 8};
    const std::array<Mode, 4> modes{Mode::Inference, Mode::Refresh, Mode::Full, Mode::Incremental};
    for (const Mode mode : modes) {
        double one_thread_rate = 0.0;
        for (const unsigned threads : thread_counts) {
            const std::uint64_t operations = run_mode(evaluator, positions, mode, threads, duration);
            const double rate = static_cast<double>(operations) * 1000.0 / duration.count();
            if (threads == 1) one_thread_rate = rate;
            const double efficiency = one_thread_rate == 0.0 ? 0.0 : rate / (one_thread_rate * threads);
            std::cout << std::fixed << std::setprecision(2)
                      << "nnue_scaling evaluator=" << label
                      << " mode=" << mode_name(mode)
                      << " threads=" << threads
                      << " operations=" << operations
                      << " ops_per_second=" << rate
                      << " efficiency=" << efficiency << '\n';
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: nnue_thread_scaling <network> [milliseconds]\n";
        return 2;
    }
    const auto duration = std::chrono::milliseconds(argc == 3 ? std::strtol(argv[2], nullptr, 10) : 1000);
    if (duration.count() <= 0) return 2;
    Attacks::initialize();
    std::string error;
    auto scalar = NetworkEvaluator::create_scalar_oracle(argv[1], error);
    if (!scalar) {
        std::cerr << error << '\n';
        return 1;
    }
    auto dispatched = NetworkEvaluator::create(argv[1], error);
    if (!dispatched) {
        std::cerr << error << '\n';
        return 1;
    }
    const auto positions = load_positions();
    benchmark_evaluator("scalar", *scalar, positions, duration);
    benchmark_evaluator(dispatched->uses_avx2() ? "avx2" : "scalar_fallback", *dispatched, positions, duration);
    return 0;
}
