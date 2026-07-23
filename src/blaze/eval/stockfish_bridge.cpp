#include "blaze/eval/stockfish_bridge.h"

#include "bitboard.h"
#include "misc.h"
#include "position.h"
#include "types.h"
#include "nnue/network.h"
#include "nnue/nnue_accumulator.h"

#include <memory>

namespace {

using namespace Stockfish;

using BigNetwork = Eval::NNUE::NetworkBig;
constexpr Eval::NNUE::IndexType FTDimensions = 1024;

struct GlobalState {
    BigNetwork network;
    Eval::NNUE::AccumulatorCaches::Cache<FTDimensions> cache;

    explicit GlobalState(const std::string& path) :
        network(Eval::NNUE::EvalFile{Stockfish::FixedString<256>(""),
                                     Stockfish::FixedString<256>(""),
                                     Stockfish::FixedString<256>("")},
                Eval::NNUE::EmbeddedNNUEType::BIG)
    {
        Bitboards::init();
        Position::init();
        network.load("", path);
        cache.clear(network);
    }
};

GlobalState* g_state = nullptr;

int evaluate_state(const std::string& fen) {
    thread_local Eval::NNUE::AccumulatorCaches::Cache<FTDimensions> tls_cache{};
    thread_local bool tls_cache_ready = false;
    if (!tls_cache_ready) {
        tls_cache.clear(g_state->network);
        tls_cache_ready = true;
    }

    Position pos;
    StateInfo si;
    pos.set(fen, false, &si);

    auto stack = std::make_unique<Eval::NNUE::AccumulatorStack>();
    stack->reset();
    stack->push();

    const auto result = g_state->network.evaluate(pos, *stack, tls_cache);
    const auto [psqt, positional] = result;

    constexpr int OutputScale = 16;
    return (static_cast<int>(psqt) + static_cast<int>(positional)) / OutputScale;
}

}

bool blaze::sf_nnue_init(std::string_view path, std::string& error) {
    sf_nnue_destroy();
    try {
        g_state = new GlobalState(std::string(path));
    } catch (const std::exception& e) {
        error = e.what();
        sf_nnue_destroy();
        return false;
    }
    return true;
}

void blaze::sf_nnue_destroy() {
    delete g_state;
    g_state = nullptr;
}

int blaze::sf_nnue_evaluate(const std::string& fen) {
    if (!g_state) return 0;
    return evaluate_state(fen);
}
