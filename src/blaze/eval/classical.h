#ifndef BLAZE_EVAL_CLASSICAL_H
#define BLAZE_EVAL_CLASSICAL_H

#include "blaze/core/position.h"

namespace blaze {

inline constexpr int search_mate_score = 32000;
inline constexpr int search_mate_threshold = 30000;

// Returns centipawns from the point of view of the side to move.
[[nodiscard]] int evaluate(const Position& position);

}  // namespace blaze

#endif  // BLAZE_EVAL_CLASSICAL_H
