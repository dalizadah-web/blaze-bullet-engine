#include "blaze/search/transposition_table.h"

#include "test_support.h"

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <thread>
#include <vector>

namespace blaze {
namespace {

static_assert(std::is_trivially_copyable_v<TTData>);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

TEST_CASE(transposition_table_round_trips_complete_entry) {
    TranspositionTable table(1);
    const Move move(Square::E2, Square::E4, MoveFlag::DoublePush);
    table.store(0x123456789abcdef0ULL, move, 37, 8, Bound::Exact, 2, 0);
    const auto hit = table.probe(0x123456789abcdef0ULL, 2);
    CHECK(hit.has_value());
    CHECK_EQ(hit->move, move);
    CHECK_EQ(hit->score, 37);
    CHECK_EQ(hit->depth, 8);
    CHECK_EQ(hit->bound, Bound::Exact);
}

TEST_CASE(transposition_table_preserves_rule50_identity) {
    TranspositionTable table(1);
    const Move move(Square::E2, Square::E4, MoveFlag::DoublePush);
    table.store(99, move, 120, 8, Bound::Lower, 2, 7);
    const auto hit = table.probe(99, 2);
    CHECK(hit.has_value());
    CHECK_EQ(hit->rule50, 7);
}

TEST_CASE(transposition_table_round_trips_static_evaluation_and_pv_metadata) {
    TranspositionTable table(1);
    const Move move(Square::E2, Square::E4, MoveFlag::DoublePush);
    table.store(99, move, 120, 8, Bound::Lower, 2, 7, 431, true);
    const auto hit = table.probe(99, 2, 7);
    CHECK(hit.has_value());
    CHECK_EQ(hit->static_evaluation, 431);
    CHECK(hit->pv);
    CHECK_EQ(hit->rule50, 7);
}

TEST_CASE(transposition_table_prefers_exact_rule50_hit_but_keeps_mismatch_for_ordering) {
    TranspositionTable table(1);
    const Move old_move(Square::A2, Square::A3);
    const Move new_move(Square::B2, Square::B3);
    table.store(123, old_move, 20, 8, Bound::Lower, 0, 4, 100, false);
    table.store(123, new_move, 30, 6, Bound::Exact, 0, 9, 200, true);
    const auto exact = table.probe(123, 0, 9);
    CHECK(exact.has_value());
    CHECK_EQ(exact->move, new_move);
    CHECK_EQ(exact->rule50, 9);
    const auto fallback = table.probe(123, 0, 7);
    CHECK(fallback.has_value());
    CHECK_EQ(fallback->move, new_move);
    CHECK_EQ(fallback->rule50, 9);
}

TEST_CASE(transposition_table_refreshes_a_different_rule50_count_even_when_shallower) {
    TranspositionTable table(1);
    table.store(456, Move(Square::A2, Square::A3), 20, 16, Bound::Exact, 0, 3, 100, true);
    table.store(456, Move(Square::B2, Square::B3), 30, 4, Bound::Lower, 0, 8, 200, false);
    const auto hit = table.probe(456, 0, 8);
    CHECK(hit.has_value());
    CHECK_EQ(hit->move, Move(Square::B2, Square::B3));
    CHECK_EQ(hit->depth, 4);
    CHECK_EQ(hit->rule50, 8);
}

TEST_CASE(transposition_table_replaces_the_weakest_colliding_entry) {
    TranspositionTable table(1);
    for (int depth = 1; depth <= 4; ++depth) {
        table.store(
            static_cast<std::uint64_t>(depth),
            Move(Square::A2, Square::A3),
            depth,
            depth,
            Bound::Upper,
            0,
            0);
    }
    table.store(5, Move(Square::B2, Square::B3), 20, 10, Bound::Exact, 0, 0);
    CHECK(!table.probe(1, 0).has_value());
    CHECK(table.probe(2, 0).has_value());
    CHECK(table.probe(5, 0).has_value());
}

TEST_CASE(transposition_table_age_lowers_an_older_colliding_entry) {
    TranspositionTable table(1);
    for (std::uint64_t key = 1; key <= 4; ++key) {
        table.store(key, Move(Square::A2, Square::A3), 1, 8, Bound::Upper, 0, 0);
    }
    for (int generation = 0; generation < 4; ++generation) {
        table.new_search();
    }
    table.store(4, Move(Square::B2, Square::B3), 1, 8, Bound::Upper, 0, 0);
    table.store(5, Move(Square::C2, Square::C3), 1, 8, Bound::Upper, 0, 0);
    CHECK(!table.probe(1, 0).has_value());
    CHECK(table.probe(4, 0).has_value());
}

TEST_CASE(transposition_table_exact_and_pv_entries_are_protected_in_replacement) {
    TranspositionTable table(1);
    table.store(1, Move(Square::A2, Square::A3), 1, 8, Bound::Upper, 0, 0);
    table.store(2, Move(Square::B2, Square::B3), 1, 8, Bound::Exact, 0, 0);
    table.store(3, Move(Square::C2, Square::C3), 1, 8, Bound::Upper, 0, 0);
    table.store(4, Move(Square::D2, Square::D3), 1, 8, Bound::Upper, 0, 0, tt_no_static_evaluation, true);
    table.store(5, Move(Square::E2, Square::E3), 1, 8, Bound::Upper, 0, 0);
    CHECK(!table.probe(1, 0).has_value());
    CHECK(table.probe(2, 0).has_value());
    CHECK(table.probe(4, 0).has_value());
}

TEST_CASE(transposition_table_hashfull_tracks_current_generation) {
    TranspositionTable table(1);
    for (std::uint64_t key = 789; key < 853; ++key) {
        table.store(key, Move(Square::A2, Square::A3), 1, 2, Bound::Exact, 0, 0);
    }
    CHECK(table.hashfull() > 0);
    table.new_search();
    CHECK_EQ(table.hashfull(), 0);
}

TEST_CASE(transposition_table_clamps_negative_rule50_to_zero) {
    TranspositionTable table(1);
    table.store(100, Move(Square::A2, Square::A3), 10, 2, Bound::Exact, 0, -1);
    const auto hit = table.probe(100, 0);
    CHECK(hit.has_value());
    CHECK_EQ(hit->rule50, 0);
}

TEST_CASE(transposition_table_clamps_rule50_above_one_hundred) {
    TranspositionTable table(1);
    table.store(101, Move(Square::B2, Square::B3), 10, 2, Bound::Exact, 0, 101);
    const auto hit = table.probe(101, 0);
    CHECK(hit.has_value());
    CHECK_EQ(hit->rule50, 100);
}

TEST_CASE(transposition_table_distinguishes_bounds_and_misses) {
    TranspositionTable table(1);
    table.store(1, Move(Square::A2, Square::A3), -12, 4, Bound::Upper, 0, 0);
    const auto hit = table.probe(1, 0);
    CHECK(hit.has_value());
    CHECK_EQ(hit->bound, Bound::Upper);
    CHECK(!table.probe(2, 0).has_value());
}

TEST_CASE(transposition_table_normalizes_mate_distance) {
    TranspositionTable table(1);
    table.store(9, Move(Square::H2, Square::H3), 31990, 12, Bound::Lower, 5, 0);
    const auto same_ply = table.probe(9, 5);
    const auto earlier_ply = table.probe(9, 2);
    CHECK(same_ply.has_value());
    CHECK(earlier_ply.has_value());
    CHECK_EQ(same_ply->score, 31990);
    CHECK_EQ(earlier_ply->score, 31993);
}

TEST_CASE(transposition_table_clear_and_resize_remove_entries) {
    TranspositionTable table(1);
    table.store(7, Move(Square::B1, Square::C3), 5, 3, Bound::Exact, 0, 0);
    CHECK(table.capacity() > 0);
    table.clear();
    CHECK(!table.probe(7, 0).has_value());
    const std::size_t old_capacity = table.capacity();
    table.resize(2);
    CHECK(table.capacity() > old_capacity);
    CHECK(!table.probe(7, 0).has_value());
}

TEST_CASE(transposition_table_uses_power_of_two_clusters_and_reports_occupancy) {
    TranspositionTable table(1);
    CHECK(table.capacity() >= 4);
    CHECK((table.capacity() & (table.capacity() - 1U)) == 0U);
    CHECK_EQ(table.hashfull(), 0);
    table.prefetch(0x123456789abcdef0ULL);
    for (std::uint64_t key = 1; key <= 64; ++key) {
        table.store(
            key,
            Move(Square::A2, Square::A3),
            static_cast<int>(key),
            4,
            Bound::Exact,
            0,
            0);
    }
    CHECK(table.hashfull() > 0);
    CHECK(table.hashfull() <= 1000);
}

TEST_CASE(transposition_table_generation_preserves_new_deep_entries) {
    TranspositionTable table(1);
    const Move deep(Square::G1, Square::F3);
    table.store(42, deep, 20, 14, Bound::Exact, 0, 0);
    table.new_search();
    table.store(42, Move(Square::G1, Square::H3), 10, 2, Bound::Upper, 0, 0);
    const auto hit = table.probe(42, 0);
    CHECK(hit.has_value());
    CHECK_EQ(hit->move, deep);
    CHECK_EQ(hit->depth, 14);
}

TEST_CASE(transposition_table_survives_concurrent_probe_store_stress) {
    TranspositionTable table(4);
    std::atomic<bool> coherent = true;
    std::vector<std::thread> workers;
    for (std::uint64_t worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&, worker] {
            for (std::uint64_t iteration = 0; iteration < 125000; ++iteration) {
                const std::uint64_t key = (iteration * 17 + worker * 131) % 4096 + 1;
                const int expected = static_cast<int>(key % 2001) - 1000;
                const int expected_depth = static_cast<int>(key % 32);
                const int expected_rule50 = static_cast<int>(key % 101);
                const Move move(
                    static_cast<Square>(key % 8),
                    static_cast<Square>(8 + key % 8));
                table.store(
                    key,
                    move,
                    expected,
                    expected_depth,
                    Bound::Exact,
                    0,
                    expected_rule50,
                    static_cast<int>(key % 30000) - 15000,
                    (key & 1U) != 0U);
                const auto hit = table.probe(key, 0);
                if (hit && (hit->score != expected || hit->move != move ||
                            hit->depth != expected_depth || hit->bound != Bound::Exact ||
                            hit->rule50 != expected_rule50 ||
                            hit->static_evaluation != static_cast<int>(key % 30000) - 15000 ||
                            hit->pv != ((key & 1U) != 0U))) {
                    coherent = false;
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(coherent.load());
}

}  // namespace
}  // namespace blaze
