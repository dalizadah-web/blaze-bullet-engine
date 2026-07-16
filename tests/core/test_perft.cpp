#include "test_support.h"

#include "blaze/core/perft.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace {

blaze::Position position_from(std::string_view fen) {
    auto position = blaze::Position::from_fen(fen);
    CHECK(position.has_value());
    return *position;
}

}  // namespace

TEST_CASE(start_position_matches_canonical_perft_through_depth_five) {
    auto position = position_from(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    constexpr std::array<std::uint64_t, 6> expected{
        1ULL,
        20ULL,
        400ULL,
        8902ULL,
        197281ULL,
        4865609ULL};

    for (int depth = 0; depth <= 5; ++depth) {
        CHECK_EQ(blaze::perft(position, depth), expected[static_cast<std::size_t>(depth)]);
    }
}

TEST_CASE(kiwipete_matches_canonical_perft_through_depth_four) {
    auto position = position_from(
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    constexpr std::array<std::uint64_t, 5> expected{
        1ULL,
        48ULL,
        2039ULL,
        97862ULL,
        4085603ULL};

    for (int depth = 0; depth <= 4; ++depth) {
        CHECK_EQ(blaze::perft(position, depth), expected[static_cast<std::size_t>(depth)]);
    }
}
