#ifndef BLAZE_SEARCH_STACK_H
#define BLAZE_SEARCH_STACK_H

#include "blaze/core/move.h"

#include <array>

namespace blaze {

struct SearchStackEntry {
    Move current_move;
    int static_evaluation = 0;
    int extension_count = 0;
    std::array<Move, 2> killers{};
    Move excluded_move;
    const void* continuation_history = nullptr;
};

}  // namespace blaze

#endif  // BLAZE_SEARCH_STACK_H
