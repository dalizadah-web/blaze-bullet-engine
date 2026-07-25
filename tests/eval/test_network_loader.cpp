#include "blaze/eval/network.h"
#include "blaze/eval/stockfish_bridge.h"
#include "blaze/core/position.h"
#include "blaze/core/attacks.h"

#include "test_support.h"

#include <string>

namespace blaze {
namespace {

TEST_CASE(network_evaluator_rejects_missing_file) {
    std::string error;
    const auto evaluator = NetworkEvaluator::create(
        "build/blaze/does-not-exist.nnue", error);
    CHECK(!evaluator.has_value());
    CHECK(!error.empty());
}

// Ensure that a valid network file loads and produces non-trivial
// evaluation (the stockfish_bridge must be live for evaluate() calls).
TEST_CASE(network_evaluator_valid_load_smoke) {
    Attacks::initialize();
    std::string error;
    const auto evaluator = NetworkEvaluator::create(
        "nn-c288c895ea92.nnue", error);
    CHECK(evaluator.has_value());
    auto pos = Position::from_fen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(evaluator->evaluate(*pos) != 0);
}

}  // namespace
}  // namespace blaze
