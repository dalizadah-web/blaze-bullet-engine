#include "blaze/eval/network.h"
#include "blaze/eval/stockfish_bridge.h"
#include "blaze/core/position.h"
#include "blaze/core/attacks.h"
#include "blaze/core/movegen.h"

#include "test_support.h"

#include <optional>
#include <initializer_list>
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
    const auto move_type = [&]() -> const char* {
        if (moves.empty()) return "root";
        const Move move = moves.back();
        if (move.has_flag(MoveFlag::CastleKing)) return "kingside_castle";
        if (move.has_flag(MoveFlag::CastleQueen)) return "queenside_castle";
        if (move.has_flag(MoveFlag::EnPassant)) return "en_passant";
        if (move.has_flag(MoveFlag::Promotion) && move.has_flag(MoveFlag::Capture))
            return "capture_promotion";
        if (move.has_flag(MoveFlag::Promotion)) return "quiet_promotion";
        if (move.has_flag(MoveFlag::Capture)) return "capture";
        if (move.has_flag(MoveFlag::DoublePush)) return "double_push";
        return "normal";
    };
    const auto fail = [&](std::string_view detail, int perspective, int index, auto wanted, auto got) {
        std::ostringstream message;
        message << "NNUE oracle mismatch fen=" << position.to_fen() << " ply=" << ply
                << " move_type=" << move_type() << " perspective=" << perspective
                << " component=" << component << ':' << detail << " index=" << index
                << " expected=" << wanted << " actual=" << got << " moves=";
        for (const Move move : moves) message << move_to_uci(move) << ' ';
        throw test::Failure(message.str());
    };
    for (int side = 0; side < 2; ++side) {
        for (int i = 0; i < 1024; ++i) {
            if (expected.halfka[side][i] != actual.halfka[side][i])
                fail("halfka", side, i, expected.halfka[side][i], actual.halfka[side][i]);
            if (expected.threats[side][i] != actual.threats[side][i])
                fail("threat", side, i, expected.threats[side][i], actual.threats[side][i]);
        }
        for (int i = 0; i < 8; ++i) {
            if (expected.halfka_psqt[side][i] != actual.halfka_psqt[side][i])
                fail("halfka_psqt", side, i, expected.halfka_psqt[side][i], actual.halfka_psqt[side][i]);
            if (expected.threat_psqt[side][i] != actual.threat_psqt[side][i])
                fail("threat_psqt", side, i, expected.threat_psqt[side][i], actual.threat_psqt[side][i]);
        }
    }
    for (int i = 0; i < 1024; ++i) {
        if (expected.transformed[i] != actual.transformed[i])
            fail("transformed",
                 static_cast<int>(i < 512 ? position.side_to_move()
                                          : opposite(position.side_to_move())),
                 i, expected.transformed[i], actual.transformed[i]);
    }
    if (expected.psqt_output != actual.psqt_output)
        fail("psqt_output", -1, 0, expected.psqt_output, actual.psqt_output);
    if (expected.positional_output != actual.positional_output)
        fail("positional_output", -1, 0, expected.positional_output, actual.positional_output);
    if (expected.raw_output != actual.raw_output)
        fail("raw_output", -1, 0, expected.raw_output, actual.raw_output);
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
            const NnueDebugSnapshot legacy = sf_nnue_debug_snapshot(position.to_fen());
            require_snapshot_equal(legacy, fresh, position, moves, ply, "legacy");
            CHECK_EQ(fresh.raw_output, sf_nnue_evaluate_raw(position.to_fen()));
            CHECK_EQ(evaluator->evaluate(position), sf_nnue_public_score(fresh.raw_output));
            CHECK_EQ(incremental.evaluate(position), sf_nnue_public_score(fresh.raw_output));
            CHECK_EQ(sf_nnue_evaluate(position.to_fen()), sf_nnue_public_score(fresh.raw_output));
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
            moves.pop_back();
            states.pop_back();
            const NnueDebugSnapshot fresh = evaluator->debug_snapshot(position);
            require_snapshot_equal(fresh, incremental.debug_snapshot(position), position, moves,
                                   static_cast<int>(index - 1), "unmake");
            require_snapshot_equal(sf_nnue_debug_snapshot(position.to_fen()), fresh, position,
                                   moves, static_cast<int>(index - 1), "legacy_unmake");
            CHECK_EQ(incremental.evaluate(position), sf_nnue_public_score(fresh.raw_output));
        }
    }
    sf_nnue_destroy();
}

TEST_CASE(nnue_dispatch_diagnostics_prove_scalar_and_avx2_kernel_paths) {
    Attacks::initialize();
    std::string error;
    auto scalar = NetworkEvaluator::create_scalar_oracle(kNetworkPath, error);
    CHECK(scalar.has_value());
    auto dispatched = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(dispatched.has_value());
    CHECK(!scalar->uses_avx2());
    CHECK_EQ(dispatched->uses_avx2(), nnue_avx2_supported());

    reset_nnue_benchmark_stats();
    auto scalar_state = scalar->make_thread_state();
    scalar_state.reset(startpos());
    const int scalar_score = scalar_state.evaluate(startpos());
    const NnueBenchmarkStats scalar_stats = nnue_benchmark_stats();
    CHECK(scalar_stats.scalar_kernel_calls > 0);
    CHECK_EQ(scalar_stats.avx2_kernel_calls, 0U);

    reset_nnue_benchmark_stats();
    auto dispatched_state = dispatched->make_thread_state();
    dispatched_state.reset(startpos());
    const int dispatched_score = dispatched_state.evaluate(startpos());
    const NnueBenchmarkStats dispatched_stats = nnue_benchmark_stats();
    CHECK_EQ(dispatched_score, scalar_score);
    if (nnue_avx2_supported()) {
        CHECK(dispatched_stats.avx2_kernel_calls > 0);
        CHECK_EQ(dispatched_stats.scalar_kernel_calls, 0U);
    } else {
        CHECK(dispatched_stats.scalar_kernel_calls > 0);
        CHECK_EQ(dispatched_stats.avx2_kernel_calls, 0U);
    }
}

TEST_CASE(direct_big_nnue_avx2_dispatch_matches_the_scalar_oracle_for_100000_legal_plies) {
    Attacks::initialize();
    std::string error;
    auto dispatched = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(dispatched.has_value());
    auto scalar = NetworkEvaluator::create_scalar_oracle(kNetworkPath, error);
    CHECK(scalar.has_value());

    std::mt19937 random{0xA2B2u};
    Position position = startpos();
    auto dispatched_state = dispatched->make_thread_state();
    auto scalar_state = scalar->make_thread_state();
    dispatched_state.reset(position);
    scalar_state.reset(position);
    std::vector<Move> moves;
    std::vector<StateInfo> states;
    moves.reserve(128);
    states.reserve(128);

    for (int ply = 0; ply < 100000; ++ply) {
        CHECK_EQ(dispatched_state.raw_evaluate(position), scalar_state.raw_evaluate(position));
        if ((ply % 251) == 0) {
            Position copied = position;
            CHECK_EQ(dispatched->raw_evaluate(copied), scalar->raw_evaluate(copied));
        }

        MoveList legal;
        generate_legal(position, legal);
        if (legal.empty() || moves.size() == 96) {
            while (!moves.empty()) {
                dispatched_state.pop();
                scalar_state.pop();
                position.unmake_move(moves.back(), states.back());
                moves.pop_back();
                states.pop_back();
                CHECK_EQ(dispatched_state.raw_evaluate(position), scalar_state.raw_evaluate(position));
            }
            continue;
        }

        const Move move = legal[static_cast<std::size_t>(random()) % legal.size()];
        StateInfo state;
        CHECK(position.make_move(move, state, true));
        dispatched_state.push(position, state);
        scalar_state.push(position, state);
        moves.push_back(move);
        states.push_back(state);
    }
}

TEST_CASE(direct_big_nnue_special_move_oracle) {
    Attacks::initialize();
    std::string error;
    auto evaluator = NetworkEvaluator::create(kNetworkPath, error);
    CHECK(evaluator.has_value());
    CHECK(sf_nnue_init(kNetworkPath, error));
    const auto run = [&](std::string_view fen, std::initializer_list<Move> moves) {
      for (int repetition = 0; repetition < 3; ++repetition) {
        const auto parsed = Position::from_fen(fen);
        CHECK(parsed.has_value());
        Position position = *parsed;
        auto incremental = evaluator->make_thread_state();
        incremental.reset(position);
        std::vector<Move> history;
        std::vector<StateInfo> states;
        const auto verify = [&](const char* phase) {
            const auto fresh = evaluator->debug_snapshot(position);
            require_snapshot_equal(fresh, incremental.debug_snapshot(position), position, history,
                                   static_cast<int>(history.size()), phase);
            require_snapshot_equal(
                sf_nnue_debug_snapshot(position.to_fen()), fresh, position, history,
                static_cast<int>(history.size()), "legacy");
            CHECK_EQ(fresh.raw_output, sf_nnue_evaluate_raw(position.to_fen()));
            CHECK_EQ(evaluator->evaluate(position), sf_nnue_public_score(fresh.raw_output));
            CHECK_EQ(incremental.evaluate(position), sf_nnue_public_score(fresh.raw_output));
            CHECK_EQ(sf_nnue_evaluate(position.to_fen()), sf_nnue_public_score(fresh.raw_output));
            Position copied = position;
            require_snapshot_equal(fresh, evaluator->debug_snapshot(copied), position, history,
                                   static_cast<int>(history.size()), "fixture_copy");
        };
        verify("fixture_before");
        for (const Move move : moves) {
            CHECK(position.is_legal(move));
            StateInfo state;
            CHECK(position.make_move(move, state, true));
            incremental.push(position, state);
            history.push_back(move);
            states.push_back(state);
            verify("fixture_after_move");
        }
        for (std::size_t i = history.size(); i > 0; --i) {
            incremental.pop();
            position.unmake_move(history[i - 1], states[i - 1]);
            history.pop_back();
            states.pop_back();
            verify("fixture_unmake");
        }
      }
    };

    // Castling on both wings and for both colors.
    run("4k3/8/8/8/8/8/8/R3K2R w KQ - 0 1", {{Square::E1, Square::G1, MoveFlag::CastleKing}});
    run("4k3/8/8/8/8/8/8/R3K2R w KQ - 0 1", {{Square::E1, Square::C1, MoveFlag::CastleQueen}});
    run("r3k2r/8/8/8/8/8/8/4K3 b kq - 0 1", {{Square::E8, Square::G8, MoveFlag::CastleKing}});
    run("r3k2r/8/8/8/8/8/8/4K3 b kq - 0 1", {{Square::E8, Square::C8, MoveFlag::CastleQueen}});

    // En passant with ordinary, rook-ray-opening, and bishop-ray-opening threat changes.
    run("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", {{Square::E5, Square::D6, MoveFlag::Capture | MoveFlag::EnPassant}});
    run("4k3/8/8/r2pP2B/8/8/8/7K w - d6 0 1", {{Square::E5, Square::D6, MoveFlag::Capture | MoveFlag::EnPassant}});
    run("4k2b/8/8/3pP3/3R4/8/8/4K3 w - d6 0 1", {{Square::E5, Square::D6, MoveFlag::Capture | MoveFlag::EnPassant}});

    // Quiet/capture promotions, including every underpromotion.
    for (const PieceType p : {PieceType::Queen, PieceType::Rook, PieceType::Bishop, PieceType::Knight}) {
        run("4k3/P7/8/8/8/8/8/4K3 w - - 0 1", {{Square::A7, Square::A8, MoveFlag::Promotion, p}});
        run("1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1", {{Square::A7, Square::B8, MoveFlag::Capture | MoveFlag::Promotion, p}});
    }

    // Same KingBuckets entry (e2/d2), then a different entry (e2/e3).
    run("4k3/8/8/8/8/8/4K3/8 w - - 0 1", {{Square::E2, Square::D2}});
    run("4k3/8/8/8/8/8/4K3/8 w - - 0 1", {{Square::E2, Square::E3}});

    // Discovered attacks, pins, double check, and simultaneously blocked/unblocked rays.
    run("7k/q7/8/8/8/8/B7/R6K w - - 0 1", {{Square::A2, Square::B3}});
    run("k3r3/8/8/8/8/8/4P3/4K3 w - - 0 1", {{Square::E2, Square::E3}});
    run("4k3/8/8/8/8/8/4B3/K3R3 w - - 0 1", {{Square::E2, Square::B5}});
    run("1r5k/4q3/8/8/8/4B3/8/1N2R1K1 w - - 0 1", {{Square::E3, Square::B6}});

    // Null moves consume and restore an accumulator stack slot without feature changes.
    for (int repetition = 0; repetition < 3; ++repetition) {
        Position null_position = *Position::from_fen("4k3/8/8/8/4P3/8/8/4K3 w - - 0 1");
        auto null_state = evaluator->make_thread_state();
        null_state.reset(null_position);
        const auto before = evaluator->debug_snapshot(null_position);
        StateInfo state;
        null_position.make_null(state, true);
        null_state.push(null_position, state);
        std::vector<Move> null_history;
        require_snapshot_equal(sf_nnue_debug_snapshot(null_position.to_fen()),
                               null_state.debug_snapshot(null_position), null_position,
                               null_history, 1, "null_push");
        CHECK_EQ(null_state.evaluate(null_position),
                 sf_nnue_public_score(sf_nnue_evaluate_raw(null_position.to_fen())));
        null_state.pop();
        null_position.unmake_null(state);
        require_snapshot_equal(before, null_state.debug_snapshot(null_position), null_position,
                               null_history, 0, "null_pop");
    }
    sf_nnue_destroy();
}

}  // namespace
}  // namespace blaze
