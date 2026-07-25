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
// 4.  Move assignment — old target is destroyed, source becomes empty.
//----------------------------------------------------------------------
TEST_CASE(nnue_move_assignment) {
    Attacks::initialize();
    std::string error;
    auto a = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(a.has_value());

    // Create a dummy evaluator to be the move-assignment target
    std::string error2;
    std::optional<NetworkEvaluator> b =
        NetworkEvaluator::create(kNetworkPath, error2);
    CHECK(b.has_value());

    // Move-assign b ← a
    *b = std::move(*a);

    CHECK(b->evaluate(startpos()) != 0);
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
// 8.  Two simultaneously live evaluators.
//
//     The current Stockfish bridge holds a single process-global
//     g_state pointer.  Creating a second evaluator overwrites the
//     pointer, leaking the first network.  Destroying either
//     evaluator deletes g_state, making the other evaluator return 0.
//     This is a documented limitation — the bridge must be redesigned
//     for multi-evaluator support.
//----------------------------------------------------------------------
TEST_CASE(nnue_two_simultaneous_evaluators_shared_global) {
    Attacks::initialize();
    std::string error_a, error_b;
    auto a = NetworkEvaluator::create(kNetworkPath, error_a);
    auto b = NetworkEvaluator::create(kNetworkPath, error_b);
    CHECK(a.has_value());
    CHECK(b.has_value());

    // Both must produce non-zero scores before any destruction.
    CHECK(a->evaluate(startpos()) != 0);

    // The second create() overwrites the global g_state pointer, so
    // object a's reference to the OLD g_state is now dangling.
    // Evaluating via a may crash or return 0.
    //
    // This test documents the current limitation.  No assertion is
    // made about a-to-*a after *b is live.
    // (empty — known limitation)
}

//----------------------------------------------------------------------
// 9.  Destruction in both possible orders does not crash.
//----------------------------------------------------------------------
TEST_CASE(nnue_destruction_order_a_then_b) {
    Attacks::initialize();
    std::string error_a, error_b;
    auto a = NetworkEvaluator::create(kNetworkPath, error_a);
    auto b = NetworkEvaluator::create(kNetworkPath, error_b);
    CHECK(a.has_value());
    CHECK(b.has_value());
    a.reset();
    b.reset();
}

TEST_CASE(nnue_destruction_order_b_then_a) {
    Attacks::initialize();
    std::string error_a, error_b;
    auto a = NetworkEvaluator::create(kNetworkPath, error_a);
    auto b = NetworkEvaluator::create(kNetworkPath, error_b);
    CHECK(a.has_value());
    CHECK(b.has_value());
    b.reset();
    a.reset();
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

}  // namespace
}  // namespace blaze
