#include "blaze/eval/network.h"
#include "blaze/eval/classical.h"
#include "blaze/eval/stockfish_bridge.h"

#include <algorithm>
#include <memory>
#include <string>

namespace blaze {

struct NetworkEvaluator::Impl {};

std::optional<NetworkEvaluator> NetworkEvaluator::create(
    std::string_view path, std::string& error) {
    error.clear();
    if (!sf_nnue_init(path, error)) {
        return std::nullopt;
    }
    NetworkEvaluator evaluator;
    evaluator.impl_ = std::make_unique<Impl>();
    return evaluator;
}

NetworkEvaluator::NetworkEvaluator(NetworkEvaluator&& other) noexcept = default;
NetworkEvaluator& NetworkEvaluator::operator=(NetworkEvaluator&& other) noexcept = default;
NetworkEvaluator::~NetworkEvaluator() { sf_nnue_destroy(); }

int NetworkEvaluator::evaluate(const Position& position) const {
    const std::string fen = position.to_fen();
    const int score = sf_nnue_evaluate(fen);
    return std::clamp(score, -search_mate_threshold + 1, search_mate_threshold - 1);
}

}  // namespace blaze
