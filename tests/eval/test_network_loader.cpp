#include "blaze/eval/network.h"

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

}  // namespace
}  // namespace blaze
