#include "blaze/eval/network.h"

#include "blaze/core/attacks.h"
#include "blaze/eval/classical.h"

#include "misc.h"
#include "nnue/features/full_threats.h"
#include "nnue/features/half_ka_v2_hm.h"
#include "nnue/network.h"

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <stdexcept>

namespace blaze {
namespace {

using Stockfish::Eval::NNUE::BigFeatureTransformer;
using Stockfish::Eval::NNUE::NetworkBig;
constexpr std::size_t kDimensions = 1024;
constexpr std::size_t kPlyCapacity = 132;
constexpr std::size_t kBuckets = 8;

Stockfish::Color to_sf_color(Color color) {
    return color == Color::White ? Stockfish::WHITE : Stockfish::BLACK;
}

Stockfish::Square to_sf_square(Square square) {
    return static_cast<Stockfish::Square>(square_index(square));
}

Stockfish::Piece to_sf_piece(Piece piece) {
    if (piece == Piece::None) return Stockfish::NO_PIECE;
    const int type = static_cast<int>(type_of(piece));
    return static_cast<Stockfish::Piece>(
        type + (color_of(piece) == Color::White ? 0 : 8));
}

struct DirectWeights {
    NetworkBig network;

    explicit DirectWeights(std::string_view path) :
        network(Stockfish::Eval::NNUE::EvalFile{
                    Stockfish::FixedString<256>(""),
                    Stockfish::FixedString<256>(""),
                    Stockfish::FixedString<256>("")},
                Stockfish::Eval::NNUE::EmbeddedNNUEType::BIG) {
        network.load("", std::string(path));
        if (!network.is_loaded()) {
            throw std::runtime_error("could not load the requested Big NNUE network");
        }
    }
};

struct Accumulator {
    std::array<std::array<std::int16_t, kDimensions>, 2> pieces{};
    std::array<std::array<std::int16_t, kDimensions>, 2> threats{};
    std::array<std::array<std::int32_t, kBuckets>, 2> piece_psqt{};
    std::array<std::array<std::int32_t, kBuckets>, 2> threat_psqt{};
};

Square king_square(const Position& position, Color color) {
    const Bitboard kings = position.pieces(color, PieceType::King);
    return kings == 0 ? Square::None : static_cast<Square>(std::countr_zero(kings));
}

Bitboard attacks_from(const Position& position, Piece piece, Square from) {
    switch (type_of(piece)) {
        case PieceType::Pawn: return Attacks::pawn(color_of(piece), from);
        case PieceType::Knight: return Attacks::knight(from);
        case PieceType::Bishop: return Attacks::bishop(from, position.occupied());
        case PieceType::Rook: return Attacks::rook(from, position.occupied());
        case PieceType::Queen: return Attacks::queen(from, position.occupied());
        case PieceType::King: return Attacks::king(from);
        case PieceType::None: return 0;
    }
    return 0;
}

void apply_piece_feature(
    Accumulator& accumulator,
    const BigFeatureTransformer& transformer,
    Color perspective,
    Square king,
    Piece piece,
    Square square,
    int sign) {
    const auto index = Stockfish::Eval::NNUE::Features::HalfKAv2_hm::make_index(
        to_sf_color(perspective), to_sf_square(square), to_sf_piece(piece), to_sf_square(king));
    const std::size_t side = static_cast<std::size_t>(perspective);
    for (std::size_t i = 0; i < kDimensions; ++i) {
        accumulator.pieces[side][i] = static_cast<std::int16_t>(
            accumulator.pieces[side][i] + sign * transformer.weights[index * kDimensions + i]);
    }
    for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
        accumulator.piece_psqt[side][bucket] +=
            sign * transformer.psqtWeights[index * kBuckets + bucket];
    }
}

void apply_threat_feature(
    Accumulator& accumulator,
    const BigFeatureTransformer& transformer,
    Color perspective,
    Square king,
    const StateInfo::NnueThreatChange& threat,
    int sign) {
    const auto index = Stockfish::Eval::NNUE::Features::FullThreats::make_index(
        to_sf_color(perspective), to_sf_piece(threat.attacker), to_sf_square(threat.from),
        to_sf_square(threat.to), to_sf_piece(threat.attacked), to_sf_square(king));
    if (index >= Stockfish::Eval::NNUE::Features::FullThreats::Dimensions) return;
    const std::size_t side = static_cast<std::size_t>(perspective);
    for (std::size_t i = 0; i < kDimensions; ++i) {
        accumulator.threats[side][i] = static_cast<std::int16_t>(
            accumulator.threats[side][i] + sign * transformer.threatWeights[index * kDimensions + i]);
    }
    for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
        accumulator.threat_psqt[side][bucket] +=
            sign * transformer.threatPsqtWeights[index * kBuckets + bucket];
    }
}

void refresh(Accumulator& accumulator, const DirectWeights& weights, const Position& position) {
    const auto& transformer = weights.network.transformer();
    accumulator = {};
    for (const Color perspective : {Color::White, Color::Black}) {
        const std::size_t side = static_cast<std::size_t>(perspective);
        accumulator.pieces[side] = transformer.biases;
        const Square king = king_square(position, perspective);
        for (int sq = 0; sq < 64; ++sq) {
            const Square square = static_cast<Square>(sq);
            const Piece piece = position.piece_on(square);
            if (piece != Piece::None) {
                apply_piece_feature(accumulator, transformer, perspective, king, piece, square, 1);
            }
        }
        for (int sq = 0; sq < 64; ++sq) {
            const Square from = static_cast<Square>(sq);
            const Piece attacker = position.piece_on(from);
            Bitboard targets = attacker == Piece::None ? 0
                : attacks_from(position, attacker, from) & position.occupied();
            while (targets != 0) {
                const Square to = static_cast<Square>(std::countr_zero(targets));
                targets &= targets - 1;
                apply_threat_feature(accumulator, transformer, perspective, king,
                    StateInfo::NnueThreatChange{attacker, position.piece_on(to), from, to, true}, 1);
            }
        }
    }
}

void apply_delta(
    Accumulator& accumulator,
    const DirectWeights& weights,
    const Position& position_after,
    const StateInfo::NnueDelta& delta) {
    if (delta.is_null) return;
    const auto& transformer = weights.network.transformer();
    for (const Color perspective : {Color::White, Color::Black}) {
        const Square king = king_square(position_after, perspective);
        if (delta.primary_piece == make_piece(perspective, PieceType::King)) {
            // HalfKAv2_hm is king-bucketed: a king move invalidates that side.
            refresh(accumulator, weights, position_after);
            return;
        }
        apply_piece_feature(accumulator, transformer, perspective, king,
            delta.primary_piece, delta.primary_from, -1);
        if (delta.primary_to != Square::None) {
            apply_piece_feature(accumulator, transformer, perspective, king,
                delta.primary_piece, delta.primary_to, 1);
        }
        if (delta.removed_piece != Piece::None) {
            apply_piece_feature(accumulator, transformer, perspective, king,
                delta.removed_piece, delta.removed_square, -1);
        }
        if (delta.added_piece != Piece::None) {
            apply_piece_feature(accumulator, transformer, perspective, king,
                delta.added_piece, delta.added_square, 1);
        }
        for (std::size_t i = 0; i < delta.threat_count; ++i) {
            apply_threat_feature(accumulator, transformer, perspective, king,
                delta.threats[i], delta.threats[i].added ? 1 : -1);
        }
    }
}

NnueDebugSnapshot make_snapshot(const Accumulator& accumulator, const DirectWeights& weights, const Position& position) {
    const std::size_t us = static_cast<std::size_t>(position.side_to_move());
    const std::size_t them = static_cast<std::size_t>(opposite(position.side_to_move()));
    const std::size_t bucket = (std::popcount(position.occupied()) - 1) / 4;
    NnueDebugSnapshot snapshot;
    snapshot.halfka = accumulator.pieces;
    snapshot.threats = accumulator.threats;
    snapshot.halfka_psqt = accumulator.piece_psqt;
    snapshot.threat_psqt = accumulator.threat_psqt;
    const std::array<std::size_t, 2> perspectives{us, them};
    for (std::size_t p = 0; p < 2; ++p) {
        const std::size_t offset = p * (kDimensions / 2);
        const std::size_t side = perspectives[p];
        for (std::size_t j = 0; j < kDimensions / 2; ++j) {
            const int first = std::clamp<int>(
                accumulator.pieces[side][j] + accumulator.threats[side][j], 0, 255);
            const int second = std::clamp<int>(
                accumulator.pieces[side][j + kDimensions / 2] +
                    accumulator.threats[side][j + kDimensions / 2], 0, 255);
            snapshot.transformed[offset + j] = static_cast<Stockfish::Eval::NNUE::TransformedFeatureType>(
                (first * second) / 512);
        }
    }
    snapshot.psqt_output =
        (accumulator.piece_psqt[us][bucket] - accumulator.piece_psqt[them][bucket] +
         accumulator.threat_psqt[us][bucket] - accumulator.threat_psqt[them][bucket]) / 2;
    snapshot.positional_output = weights.network.architecture(bucket).propagate(snapshot.transformed.data());
    // The legacy bridge exposed the two Network::evaluate() components after
    // their internal OutputScale division, then divided their sum once more.
    // Keep that public score contract while the bridge remains the oracle.
    // Match Network::evaluate(): each component is converted from network
    // units independently before its wrapper combines the two values.
    snapshot.raw_output = static_cast<int>(snapshot.psqt_output / Stockfish::Eval::NNUE::OutputScale) +
                          static_cast<int>(snapshot.positional_output / Stockfish::Eval::NNUE::OutputScale);
    return snapshot;
}

}  // namespace

struct NetworkEvaluator::Impl {
    explicit Impl(std::string_view path) : weights(std::make_shared<DirectWeights>(path)) {}
    std::shared_ptr<const DirectWeights> weights;
};

struct NnueThreadState::Impl {
    explicit Impl(std::shared_ptr<const DirectWeights> network) : weights(std::move(network)) {}
    std::shared_ptr<const DirectWeights> weights;
    std::array<Accumulator, kPlyCapacity> stack{};
    std::size_t ply = 0;
};

std::optional<NetworkEvaluator> NetworkEvaluator::create(std::string_view path, std::string& error) {
    error.clear();
    try {
        NetworkEvaluator evaluator;
        evaluator.impl_ = std::make_unique<Impl>(path);
        return evaluator;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

NetworkEvaluator::NetworkEvaluator(NetworkEvaluator&& other) noexcept = default;
NetworkEvaluator& NetworkEvaluator::operator=(NetworkEvaluator&& other) noexcept = default;
NetworkEvaluator::~NetworkEvaluator() = default;

NnueThreadState::NnueThreadState(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NnueThreadState::NnueThreadState(NnueThreadState&& other) noexcept = default;
NnueThreadState& NnueThreadState::operator=(NnueThreadState&& other) noexcept = default;
NnueThreadState::~NnueThreadState() = default;

NnueThreadState NetworkEvaluator::make_thread_state() const {
    return NnueThreadState{std::make_unique<NnueThreadState::Impl>(impl_->weights)};
}

void NnueThreadState::reset(const Position& position) {
    impl_->ply = 0;
    refresh(impl_->stack[0], *impl_->weights, position);
}

void NnueThreadState::push(const Position& position_after, const StateInfo& move_state) {
    if (impl_->ply + 1 >= impl_->stack.size()) return;
    impl_->stack[impl_->ply + 1] = impl_->stack[impl_->ply];
    ++impl_->ply;
    apply_delta(impl_->stack[impl_->ply], *impl_->weights, position_after, move_state.nnue);
}

void NnueThreadState::pop() {
    if (impl_->ply > 0) --impl_->ply;
}

int sf_nnue_public_score(int raw_network_output) {
    return std::clamp(raw_network_output / Stockfish::Eval::NNUE::OutputScale,
        -search_mate_threshold + 1, search_mate_threshold - 1);
}

int NnueThreadState::raw_evaluate(const Position& position) const {
    return debug_snapshot(position).raw_output;
}

NnueDebugSnapshot NnueThreadState::debug_snapshot(const Position& position) const {
    return make_snapshot(impl_->stack[impl_->ply], *impl_->weights, position);
}

int NnueThreadState::evaluate(const Position& position) const {
    return sf_nnue_public_score(raw_evaluate(position));
}

int NetworkEvaluator::evaluate(const Position& position) const {
    return sf_nnue_public_score(raw_evaluate(position));
}

int NetworkEvaluator::raw_evaluate(const Position& position) const {
    auto state = make_thread_state();
    state.reset(position);
    return state.raw_evaluate(position);
}

NnueDebugSnapshot NetworkEvaluator::debug_snapshot(const Position& position) const {
    auto state = make_thread_state();
    state.reset(position);
    return state.debug_snapshot(position);
}

}  // namespace blaze
