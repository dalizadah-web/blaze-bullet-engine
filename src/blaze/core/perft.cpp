#include "blaze/core/perft.h"

#include "blaze/core/movegen.h"

namespace blaze {

std::uint64_t perft(Position& position, int depth) {
    if (depth <= 0) {
        return 1;
    }

    MoveList moves;
    generate_legal(position, moves);
    if (depth == 1) {
        return static_cast<std::uint64_t>(moves.size());
    }

    std::uint64_t nodes = 0;
    for (const Move move : moves) {
        StateInfo state;
        if (!position.make_move(move, state)) {
            continue;
        }
        nodes += perft(position, depth - 1);
        position.unmake_move(move, state);
    }
    return nodes;
}

}  // namespace blaze
