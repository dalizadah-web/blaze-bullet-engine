#include "blaze/eval/stockfish_bridge.h"
#include "blaze/eval/network.h"

#include "bitboard.h"
#include "misc.h"
#include "position.h"
#include "types.h"
#include "nnue/network.h"
#include "nnue/nnue_accumulator.h"

#include <algorithm>
#include <array>
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
        if (!network.is_loaded()) {
            throw std::runtime_error("no valid NNUE weights loaded from '" + path + "'");
        }
        cache.clear(network);
    }
};

GlobalState* g_state = nullptr;

int evaluate_state_raw(const std::string& fen) {
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

    return static_cast<int>(psqt) + static_cast<int>(positional);
}

blaze::NnueDebugSnapshot debug_state(const std::string& fen) {
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

    constexpr std::size_t dimensions = 1024;
    alignas(Eval::NNUE::CacheLineSize) std::array<
        Eval::NNUE::TransformedFeatureType,
        Eval::NNUE::BigFeatureTransformer::BufferSize> transformed{};
    const int bucket = (pos.count<ALL_PIECES>() - 1) / 4;
    const auto& transformer = g_state->network.transformer();
    const std::int32_t psqt =
        transformer.transform(pos, *stack, tls_cache, transformed.data(), bucket);
    const std::int32_t positional =
        g_state->network.architecture(static_cast<std::size_t>(bucket))
            .propagate(transformed.data());

    blaze::NnueDebugSnapshot snapshot;
    const auto& piece_state =
        stack->latest<Eval::NNUE::PSQFeatureSet>().template acc<FTDimensions>();
    const auto& threat_state =
        stack->latest<Eval::NNUE::ThreatFeatureSet>().template acc<FTDimensions>();
    for (std::size_t side = 0; side < 2; ++side) {
        snapshot.halfka[side] = piece_state.accumulation[side];
        snapshot.threats[side] = threat_state.accumulation[side];
        snapshot.halfka_psqt[side] = piece_state.psqtAccumulation[side];
        snapshot.threat_psqt[side] = threat_state.psqtAccumulation[side];
    }
    std::copy_n(transformed.begin(), dimensions, snapshot.transformed.begin());
    snapshot.psqt_output = psqt;
    snapshot.positional_output = positional;
    snapshot.raw_output =
        static_cast<int>(psqt / Eval::NNUE::OutputScale) +
        static_cast<int>(positional / Eval::NNUE::OutputScale);
    return snapshot;
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
    return sf_nnue_public_score(sf_nnue_evaluate_raw(fen));
}

int blaze::sf_nnue_evaluate_raw(const std::string& fen) {
    if (!g_state) return 0;
    return evaluate_state_raw(fen);
}

blaze::NnueDebugSnapshot blaze::sf_nnue_debug_snapshot(const std::string& fen) {
    if (!g_state) return {};
    return debug_state(fen);
}
