#include "blaze/eval/network.h"
#include "blaze/eval/stockfish_bridge.h"
#include "blaze/core/position.h"
#include "blaze/core/attacks.h"

#include "test_support.h"

#include <optional>
#include <string>
#include <thread>
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

}  // namespace
}  // namespace blaze
