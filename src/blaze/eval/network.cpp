#include "blaze/eval/network.h"
#include "blaze/eval/classical.h"
#include "blaze/eval/stockfish_bridge.h"

#include <algorithm>
#include <memory>
#include <string>

namespace blaze {

namespace {

bool evaluator_alive = false;

}

struct NetworkEvaluator::Impl {};

std::optional<NetworkEvaluator> NetworkEvaluator::create(
    std::string_view path, std::string& error) {
    error.clear();
    if (evaluator_alive) {
        error = "a NetworkEvaluator is already alive; only one may exist at a time";
        return std::nullopt;
    }
    if (!sf_nnue_init(path, error)) {
        return std::nullopt;
    }
    auto impl = std::make_unique<Impl>();
    NetworkEvaluator evaluator;
    evaluator.impl_ = std::move(impl);
    evaluator_alive = true;
    return evaluator;
}

NetworkEvaluator::NetworkEvaluator(NetworkEvaluator&& other) noexcept = default;
NetworkEvaluator& NetworkEvaluator::operator=(NetworkEvaluator&& other) noexcept {
    if (this != &other) {
        if (impl_) sf_nnue_destroy();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
NetworkEvaluator::~NetworkEvaluator() {
    if (impl_) {
        impl_.reset();
        sf_nnue_destroy();
        evaluator_alive = false;
    }
}

int NetworkEvaluator::evaluate(const Position& position) const {
    const std::string fen = position.to_fen();
    const int score = sf_nnue_evaluate(fen);
    return std::clamp(score, -search_mate_threshold + 1, search_mate_threshold - 1);
}

}  // namespace blaze
