#ifndef BLAZE_SEARCH_SEE_H
#define BLAZE_SEARCH_SEE_H

#include "blaze/core/move.h"
#include "blaze/core/position.h"

namespace blaze {

// Returns the material gain of a capture after the complete legal exchange on
// the destination square. Quiet moves return zero. The calculation is
// intentionally independent from the main evaluator and is used for tactical
// move ordering/pruning only.
[[nodiscard]] int static_exchange_evaluation(const Position& position, Move move);

// Returns true if static_exchange_evaluation(move) >= threshold.
// More efficient than calling static_exchange_evaluation() and comparing,
// because the exchange can early-exit once the threshold is guaranteed.
[[nodiscard]] bool see_ge(const Position& position, Move move, int threshold);

}  // namespace blaze

#endif  // BLAZE_SEARCH_SEE_H
