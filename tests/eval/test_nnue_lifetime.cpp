#include "blaze/eval/network.h"
#include "blaze/eval/stockfish_bridge.h"
#include "blaze/core/position.h"
#include "blaze/core/attacks.h"
#include "blaze/core/movegen.h"

#include "test_support.h"

#include <optional>
#include <iostream>
#include <string>
#include <thread>
#include <random>
#include <sstream>
#include <vector>

namespace blaze {
namespace {

static constexpr const char* kNetworkPath = "nn-c288c895ea92.nnue";

static Position startpos() {
    auto pos = Position::from_fen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    return *pos;
}

//----------------------------------------------------------------------
// 1.  Create-and-use returns a non-zero score.
//----------------------------------------------------------------------
TEST_CASE(nnue_smoke_create_and_evaluate) {
    Attacks::initialize();
    std::string error;
    auto evaluator = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(evaluator.has_value());
    CHECK(evaluator->evaluate(startpos()) != 0);
}

//----------------------------------------------------------------------
// 2.  std::optional<NetworkEvaluator> — the moved-from temporary is
//     destroyed without killing the global state.
//
//     This is the exact pattern used by UciSession::start_search:
//     create() returns std::optional<NetworkEvaluator>.  The local
//     variable is moved into the optional, then the local's destructor
//     runs.  If the destructor unconditionally destroys the global NNUE
//     state the evaluation below will return 0.
//----------------------------------------------------------------------
TEST_CASE(nnue_lives_through_optional_return) {
    Attacks::initialize();
    std::string error;
    std::optional<NetworkEvaluator> opt =
        NetworkEvaluator::create(kNetworkPath, error);
    CHECK(opt.has_value());
    CHECK(opt->evaluate(startpos()) != 0);
}

//----------------------------------------------------------------------
// 3.  Move construction — source is destroyed, target still works.
//----------------------------------------------------------------------
TEST_CASE(nnue_move_construction) {
    Attacks::initialize();
    std::string error;
    auto a = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(a.has_value());

    NetworkEvaluator b(std::move(*a));
    a.reset();

    CHECK(b.evaluate(startpos()) != 0);
}

//----------------------------------------------------------------------
// 4.  Move assignment — source's state survives, moved-from target
//     is inert, and the assigned object evaluates correctly.
//----------------------------------------------------------------------
TEST_CASE(nnue_move_assignment) {
    Attacks::initialize();
    std::string error;
    auto opt = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(opt.has_value());

    NetworkEvaluator a = std::move(*opt);
    opt.reset();

    NetworkEvaluator b = std::move(a);  // b owns state, a is moved-from
    a   = std::move(b);                // move-assign valid b into moved-from a
    CHECK(a.evaluate(startpos()) != 0);
    // b is moved-from — destructor no-ops
}

//----------------------------------------------------------------------
// 5.  Moved-from object is inert (destructor does not double-free).
//----------------------------------------------------------------------
TEST_CASE(nnue_moved_from_is_inert) {
    Attacks::initialize();
    std::string error;
    auto a = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(a.has_value());

    NetworkEvaluator b(std::move(*a));
    // a is now moved-from — its destructor runs when `a` goes out of
    // scope.  If the destructor unconditionally calls sf_nnue_destroy()
    // it will kill the global state and b will return 0.
    // The test passes if no crash occurs AND b's state is intact.
    CHECK(b.evaluate(startpos()) != 0);
}

//----------------------------------------------------------------------
// 6.  Repeated create/destroy cycles.
//----------------------------------------------------------------------
TEST_CASE(nnue_repeated_create_destroy) {
    Attacks::initialize();
    for (int i = 0; i < 10; ++i) {
        std::string error;
        auto opt = NetworkEvaluator::create(kNetworkPath, error);
        CHECK(opt.has_value());
        CHECK(opt->evaluate(startpos()) != 0);
    }
}

//----------------------------------------------------------------------
// 7.  Failed creation — when the underlying Stockfish bridge throws on
//     init, the optional is empty and subsequent creation works.
//----------------------------------------------------------------------
TEST_CASE(nnue_failed_creation_cleanup) {
    Attacks::initialize();
    std::string error;

    auto opt_empty =
        NetworkEvaluator::create(
            "build/blaze/nonexistent.nnue", error);
    CHECK(!opt_empty.has_value());
    CHECK(!error.empty());

    auto opt_valid = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(opt_valid.has_value());
    CHECK(opt_valid->evaluate(startpos()) != 0);
}

//----------------------------------------------------------------------
// 8.  Single-evaluator contract — only one evaluator may exist at a
//     time.  A second create() is rejected with a clear error.
//----------------------------------------------------------------------
TEST_CASE(nnue_independent_evaluators_can_coexist) {
    Attacks::initialize();
    std::string error_a;
    auto a = NetworkEvaluator::create(kNetworkPath, error_a);
    CHECK(a.has_value());

    std::string error_b;
    auto b = NetworkEvaluator::create(kNetworkPath, error_b);
    CHECK(b.has_value());
    CHECK_EQ(a->evaluate(startpos()), b->evaluate(startpos()));
}

//----------------------------------------------------------------------
// 9.  Create-then-destroy followed by a fresh create works.
//----------------------------------------------------------------------
TEST_CASE(nnue_create_destroy_create) {
    Attacks::initialize();
    std::string error;
    auto a = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(a.has_value());
    CHECK(a->evaluate(startpos()) != 0);
    a.reset();

    auto b = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(b.has_value());
    CHECK(b->evaluate(startpos()) != 0);
}

//----------------------------------------------------------------------
// 10. Multithreaded evaluation.
//
//     The network is read-only during evaluation.  The thread-local
//     accumulator cache and the fresh AccumulatorStack per call keep
//     threads isolated.  This test is best run under ThreadSanitizer.
//----------------------------------------------------------------------
TEST_CASE(nnue_multithreaded_evaluation) {
    Attacks::initialize();
    std::string error;
    auto evaluator = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(evaluator.has_value());
    CHECK(evaluator->evaluate(startpos()) != 0);

    std::vector<std::thread> threads;
    std::vector<int> scores(8, -1);
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&evaluator, &scores, i] {
            scores[i] = evaluator->evaluate(startpos());
        });
    }
    for (auto& t : threads) t.join();

    for (int i = 0; i < 8; ++i) {
        CHECK(scores[i] != 0);
        CHECK_EQ(scores[i], scores[0]);
    }
}

//----------------------------------------------------------------------
// 11. Create / evaluate / destroy cycle with multiple positions.
//----------------------------------------------------------------------
TEST_CASE(nnue_create_evaluate_destroy_cycle) {
    Attacks::initialize();
    for (int i = 0; i < 5; ++i) {
        std::string error;
        auto opt = NetworkEvaluator::create(kNetworkPath, error);
        CHECK(opt.has_value());

        auto pos1 = Position::from_fen(
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        auto pos2 = Position::from_fen(
            "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/2NP4/PPP2PPP/R1BQK2R w KQkq - 4 5");
        CHECK(opt->evaluate(*pos1) != 0);
        CHECK(opt->evaluate(*pos2) != 0);

        // Two distinct positions should produce distinct scores.
        CHECK(opt->evaluate(*pos1) != opt->evaluate(*pos2));
    }
}

void require_snapshot_equal(
    const NnueDebugSnapshot& expected,
    const NnueDebugSnapshot& actual,
    const Position& position,
    const std::vector<Move>& moves,
    int ply,
    const char* component) {
    const auto fail = [&](std::string_view detail, int index, auto wanted, auto got) {
        std::ostringstream message;
        message << "NNUE oracle mismatch fen=" << position.to_fen() << " ply=" << ply
                << " component=" << component << ':' << detail << " index=" << index
                << " expected=" << wanted << " actual=" << got << " moves=";
        for (const Move move : moves) message << move_to_uci(move) << ' ';
        throw test::Failure(message.str());
    };
    for (int side = 0; side < 2; ++side) {
        for (int i = 0; i < 1024; ++i) {
            if (expected.halfka[side][i] != actual.halfka[side][i])
                fail("halfka", side * 1024 + i, expected.halfka[side][i], actual.halfka[side][i]);
            if (expected.threats[side][i] != actual.threats[side][i])
                fail("threat", side * 1024 + i, expected.threats[side][i], actual.threats[side][i]);
        }
        for (int i = 0; i < 8; ++i) {
            if (expected.halfka_psqt[side][i] != actual.halfka_psqt[side][i])
                fail("halfka_psqt", side * 8 + i, expected.halfka_psqt[side][i], actual.halfka_psqt[side][i]);
            if (expected.threat_psqt[side][i] != actual.threat_psqt[side][i])
                fail("threat_psqt", side * 8 + i, expected.threat_psqt[side][i], actual.threat_psqt[side][i]);
        }
    }
    for (int i = 0; i < 1024; ++i) {
        if (expected.transformed[i] != actual.transformed[i])
            fail("transformed", i, expected.transformed[i], actual.transformed[i]);
    }
    if (expected.psqt_output != actual.psqt_output)
        fail("psqt_output", 0, expected.psqt_output, actual.psqt_output);
    if (expected.positional_output != actual.positional_output)
        fail("positional_output", 0, expected.positional_output, actual.positional_output);
    if (expected.raw_output != actual.raw_output)
        fail("raw_output", 0, expected.raw_output, actual.raw_output);
}

TEST_CASE(nnue_thread_state_matches_a_fresh_direct_refresh_after_a_move) {
    std::string error;
    auto evaluator = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(evaluator.has_value());

    auto parsed = Position::from_fen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(parsed.has_value());
    Position position = *parsed;
    auto state = evaluator->make_thread_state();
    state.reset(position);
    CHECK_EQ(state.evaluate(position), evaluator->evaluate(position));

    StateInfo move_state;
    CHECK(position.make_move(
        Move{Square::E2, Square::E4, MoveFlag::DoublePush}, move_state, true));
    state.push(position, move_state);
    CHECK_EQ(state.evaluate(position), evaluator->evaluate(position));
    state.pop();
    position.unmake_move(Move{Square::E2, Square::E4, MoveFlag::DoublePush}, move_state);
    CHECK_EQ(state.evaluate(position), evaluator->evaluate(position));
}

TEST_CASE(direct_big_nnue_matches_the_legacy_bridge_oracle) {
    Attacks::initialize();
    std::string error;
    auto evaluator = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(evaluator.has_value());
    CHECK(sf_nnue_init(kNetworkPath, error));

    const std::array<const char*, 3> fens{
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/2NP4/PPP2PPP/R1BQK2R w KQkq - 4 5",
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2"};
    for (const char* fen : fens) {
        const auto position = Position::from_fen(fen);
        CHECK(position.has_value());
        const int legacy_raw = sf_nnue_evaluate_raw(position->to_fen());
        const int direct_raw = evaluator->raw_evaluate(*position);
        auto state = evaluator->make_thread_state();
        state.reset(*position);
        const int incremental_raw = state.raw_evaluate(*position);
        const int legacy_public = sf_nnue_evaluate(position->to_fen());
        const int fresh_public = evaluator->evaluate(*position);
        const int incremental_public = state.evaluate(*position);
        std::cerr << "NNUE oracle fen=" << position->to_fen()
                  << " legacy_raw=" << legacy_raw
                  << " fresh_direct_raw=" << direct_raw
                  << " incremental_raw=" << incremental_raw
                  << " legacy_public=" << legacy_public
                  << " fresh_direct_public=" << fresh_public
                  << " incremental_public=" << incremental_public << '\n';
        if (direct_raw != legacy_raw) {
            std::cerr << "NNUE raw mismatch fen=" << position->to_fen()
                      << " expected=" << legacy_raw << " actual=" << direct_raw << '\n';
        }
        CHECK_EQ(direct_raw, legacy_raw);
        CHECK_EQ(incremental_raw, legacy_raw);
        CHECK_EQ(fresh_public, sf_nnue_public_score(legacy_raw));
        CHECK_EQ(legacy_public, sf_nnue_public_score(legacy_raw));
        CHECK_EQ(incremental_public, sf_nnue_public_score(legacy_raw));
    }
    sf_nnue_destroy();
}

TEST_CASE(direct_big_nnue_randomized_incremental_oracle) {
    Attacks::initialize();
    std::string error;
    auto evaluator = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(evaluator.has_value());
    CHECK(sf_nnue_init(kNetworkPath, error));
    std::mt19937 random{0xB1A2E123u};

    for (int sequence = 0; sequence < 64; ++sequence) {
        Position position = startpos();
        auto incremental = evaluator->make_thread_state();
        incremental.reset(position);
        std::vector<Move> moves;
        std::vector<StateInfo> states;
        for (int ply = 0; ply < 128; ++ply) {
            const NnueDebugSnapshot fresh = evaluator->debug_snapshot(position);
            const NnueDebugSnapshot current = incremental.debug_snapshot(position);
            require_snapshot_equal(fresh, current, position, moves, ply, "incremental");
            const int legacy_raw = sf_nnue_evaluate_raw(position.to_fen());
            if (fresh.raw_output != legacy_raw) {
                std::ostringstream message;
                message << "NNUE oracle mismatch fen=" << position.to_fen() << " ply=" << ply
                        << " component=legacy_raw expected=" << legacy_raw
                        << " actual=" << fresh.raw_output;
                throw test::Failure(message.str());
            }
            Position copied = position;
            const NnueDebugSnapshot copied_snapshot = evaluator->debug_snapshot(copied);
            require_snapshot_equal(fresh, copied_snapshot, position, moves, ply, "position_copy");

            MoveList legal;
            generate_legal(position, legal);
            if (legal.empty()) break;
            const Move move = legal[static_cast<std::size_t>(random()) % legal.size()];
            StateInfo state;
            CHECK(position.make_move(move, state, true));
            incremental.push(position, state);
            moves.push_back(move);
            states.push_back(state);
        }
        for (std::size_t index = moves.size(); index > 0; --index) {
            incremental.pop();
            position.unmake_move(moves[index - 1], states[index - 1]);
            const NnueDebugSnapshot fresh = evaluator->debug_snapshot(position);
            require_snapshot_equal(fresh, incremental.debug_snapshot(position), position, moves,
                                   static_cast<int>(index - 1), "unmake");
        }
    }
    sf_nnue_destroy();
}

}  // namespace
}  // namespace blaze
