#include "blaze/search/transposition_table.h"

#include "test_support.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace blaze {
namespace {

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
    for (std::uint64_t worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            for (std::uint64_t iteration = 0; iteration < 20000; ++iteration) {
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
                    expected_rule50);
                const auto hit = table.probe(key, 0);
                if (hit && (hit->score != expected || hit->move != move ||
                            hit->depth != expected_depth || hit->bound != Bound::Exact ||
                            hit->rule50 != expected_rule50)) {
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
