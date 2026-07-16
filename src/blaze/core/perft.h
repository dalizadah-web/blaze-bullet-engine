#ifndef BLAZE_CORE_PERFT_H
#define BLAZE_CORE_PERFT_H

#include "blaze/core/position.h"

#include <cstdint>

namespace blaze {

[[nodiscard]] std::uint64_t perft(Position& position, int depth);

}  // namespace blaze

#endif  // BLAZE_CORE_PERFT_H
