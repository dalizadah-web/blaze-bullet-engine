#include "blaze/eval/classical.h"
#include "blaze/eval/network.h"
#include "blaze/core/attacks.h"
#include "blaze/core/position.h"

#include "test_support.h"

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace blaze {
namespace {

static Position pos(const char* fen) {
    return *Position::from_fen(fen);
}

static constexpr const char* kNetworkPath = "nn-c288c895ea92.nnue";

// Compare NNUE vs classical evaluation on several positions.
// NNUE is evaluated through the Stockfish bridge (FEN → parse → full forward pass).
TEST_CASE(nnue_vs_classical_eval_values) {
    Attacks::initialize();
    std::string error;
    auto evaluator = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(evaluator.has_value());

    struct TestPos {
        const char* fen;
        const char* label;
    };

    const TestPos positions[] = {
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",     "startpos"},
        {"r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/2NP4/PPP2PPP/R1BQK2R w KQkq - 4 5", "Italian"},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", "complex"},
        {"8/5k2/3p4/1p1Pp2p/pP2Pp1P/P4P1K/8/8 b - - 0 1",               "fortress"},
        {"4k2r/6r1/8/8/8/8/3R4/R3K3 w Qk - 0 1",                       "endgame"},
    };

    for (const auto& tp : positions) {
        const auto p = pos(tp.fen);
        const int nnue_score = evaluator->evaluate(p);
        const int classical_score = evaluate(p);
        // Both evaluators stay outside the mate band.
        CHECK(std::abs(nnue_score) < search_mate_threshold);
        CHECK(std::abs(classical_score) < search_mate_threshold);
        // NNUE and classical produce different scores (different eval function).
        CHECK(nnue_score != classical_score);
    }
}

// Measure the evaluation throughput difference.
// NNUE goes through FEN serialization + Stockfish parsing + full forward pass.
// Classical is a direct hand-rolled evaluation.
TEST_CASE(nnue_vs_classical_throughput) {
    Attacks::initialize();
    const auto p = pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

    // Warmup
    for (int i = 0; i < 100; ++i) {
        volatile int s = evaluate(p);
        (void)s;
    }

    constexpr int kSamples = 50000;
    auto t0 = std::chrono::steady_clock::now();
    int classical_sum = 0;
    for (int i = 0; i < kSamples; ++i) {
        classical_sum += evaluate(p);
    }
    auto t1 = std::chrono::steady_clock::now();
    const auto classical_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const double classical_per_us = static_cast<double>(kSamples) / classical_us;

    // NNUE throughput
    std::string error;
    auto evaluator = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(evaluator.has_value());
    volatile int nnue_warmup = evaluator->evaluate(p);
    (void)nnue_warmup;

    constexpr int kNnueSamples = 500;
    t0 = std::chrono::steady_clock::now();
    int nnue_sum = 0;
    for (int i = 0; i < kNnueSamples; ++i) {
        nnue_sum += evaluator->evaluate(p);
    }
    t1 = std::chrono::steady_clock::now();
    const auto nnue_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const double nnue_per_us = static_cast<double>(kNnueSamples) / nnue_us;

    const double slowdown = nnue_us > 0 && classical_us > 0
        ? static_cast<double>(nnue_us) / kNnueSamples / (static_cast<double>(classical_us) / kSamples)
        : 0.0;

    // Verify NNUE is slower (known limitation of the bridge).
    CHECK(slowdown > 10.0);

    (void)classical_sum;
    (void)nnue_sum;
}

}  // namespace
}  // namespace blaze
