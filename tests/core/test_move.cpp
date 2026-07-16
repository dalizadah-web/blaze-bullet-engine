#include "test_support.h"

#include "blaze/core/move.h"

#include <array>
#include <string_view>

using blaze::Move;
using blaze::MoveFlag;
using blaze::PieceType;
using blaze::Square;

TEST_CASE(square_numbering_is_little_endian_rank_file) {
    CHECK_EQ(static_cast<int>(Square::A1), 0);
    CHECK_EQ(static_cast<int>(Square::H1), 7);
    CHECK_EQ(static_cast<int>(Square::A8), 56);
    CHECK_EQ(static_cast<int>(Square::H8), 63);
}

TEST_CASE(square_text_round_trips) {
    CHECK_EQ(blaze::square_from_string("a1"), Square::A1);
    CHECK_EQ(blaze::square_from_string("e4"), Square::E4);
    CHECK_EQ(blaze::square_from_string("h8"), Square::H8);
    CHECK_EQ(blaze::square_to_string(Square::A1), "a1");
    CHECK_EQ(blaze::square_to_string(Square::E4), "e4");
    CHECK_EQ(blaze::square_to_string(Square::H8), "h8");
}

TEST_CASE(square_parser_rejects_invalid_coordinates) {
    CHECK_EQ(blaze::square_from_string(""), Square::None);
    CHECK_EQ(blaze::square_from_string("a"), Square::None);
    CHECK_EQ(blaze::square_from_string("i1"), Square::None);
    CHECK_EQ(blaze::square_from_string("a9"), Square::None);
    CHECK_EQ(blaze::square_from_string("A1"), Square::None);
}

TEST_CASE(normal_uci_move_round_trips) {
    const auto parsed = blaze::move_from_uci("e2e4");
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->from(), Square::E2);
    CHECK_EQ(parsed->to(), Square::E4);
    CHECK_EQ(parsed->promotion(), PieceType::None);
    CHECK_EQ(blaze::move_to_uci(*parsed), "e2e4");
}

TEST_CASE(all_uci_promotions_round_trip) {
    constexpr std::array<std::string_view, 4> moves{
        "a7a8q", "b7b8r", "c7c8b", "d7d8n"};

    for (const std::string_view text : moves) {
        const auto parsed = blaze::move_from_uci(text);
        CHECK(parsed.has_value());
        CHECK(parsed->has_flag(MoveFlag::Promotion));
        CHECK_EQ(blaze::move_to_uci(*parsed), text);
    }
}

TEST_CASE(move_encoding_preserves_flags_and_promotion) {
    const Move move{
        Square::E7,
        Square::E8,
        MoveFlag::Capture | MoveFlag::Promotion,
        PieceType::Knight};

    CHECK(move.is_valid());
    CHECK(move.has_flag(MoveFlag::Capture));
    CHECK(move.has_flag(MoveFlag::Promotion));
    CHECK_EQ(move.promotion(), PieceType::Knight);
    CHECK_EQ(move.from(), Square::E7);
    CHECK_EQ(move.to(), Square::E8);
}

TEST_CASE(uci_parser_rejects_malformed_moves) {
    CHECK(!blaze::move_from_uci("").has_value());
    CHECK(!blaze::move_from_uci("0000").has_value());
    CHECK(!blaze::move_from_uci("e2e").has_value());
    CHECK(!blaze::move_from_uci("e2e9").has_value());
    CHECK(!blaze::move_from_uci("e7e8k").has_value());
    CHECK(!blaze::move_from_uci("e2e4q").has_value());
}
