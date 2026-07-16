#include "test_support.h"

#include "blaze/core/position.h"

#include <array>
#include <string>
#include <string_view>

namespace {

blaze::Position position_from(std::string_view fen) {
    auto position = blaze::Position::from_fen(fen);
    CHECK(position.has_value());
    return *position;
}

void require_round_trip(blaze::Position position, blaze::Move move) {
    const std::string before_fen = position.to_fen();
    const std::uint64_t before_key = position.key();
    blaze::StateInfo state;

    CHECK(position.make_move(move, state));
    CHECK(position.is_consistent());
    position.unmake_move(move, state);

    CHECK(position.is_consistent());
    CHECK_EQ(position.to_fen(), before_fen);
    CHECK_EQ(position.key(), before_key);
}

}  // namespace

TEST_CASE(normal_move_updates_and_restores_all_state) {
    auto position = position_from(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    const blaze::Move move{
        blaze::Square::E2,
        blaze::Square::E4,
        blaze::MoveFlag::DoublePush};
    const std::string before = position.to_fen();
    const std::uint64_t key = position.key();
    blaze::StateInfo state;

    CHECK(position.make_move(move, state));
    CHECK_EQ(
        position.to_fen(),
        "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    CHECK(position.key() != key);
    position.unmake_move(move, state);
    CHECK_EQ(position.to_fen(), before);
    CHECK_EQ(position.key(), key);
}

TEST_CASE(capture_move_round_trips) {
    auto position = position_from("4k3/8/8/3p4/4P3/8/8/4K3 w - - 7 10");
    require_round_trip(
        position,
        blaze::Move{
            blaze::Square::E4,
            blaze::Square::D5,
            blaze::MoveFlag::Capture});
}

TEST_CASE(en_passant_move_round_trips) {
    auto position = position_from("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2");
    require_round_trip(
        position,
        blaze::Move{
            blaze::Square::E5,
            blaze::Square::D6,
            blaze::MoveFlag::Capture | blaze::MoveFlag::EnPassant});
}

TEST_CASE(both_white_castles_round_trip) {
    constexpr std::string_view fen = "4k3/8/8/8/8/8/8/R3K2R w KQ - 0 1";
    require_round_trip(
        position_from(fen),
        blaze::Move{
            blaze::Square::E1,
            blaze::Square::G1,
            blaze::MoveFlag::CastleKing});
    require_round_trip(
        position_from(fen),
        blaze::Move{
            blaze::Square::E1,
            blaze::Square::C1,
            blaze::MoveFlag::CastleQueen});
}

TEST_CASE(both_black_castles_round_trip) {
    constexpr std::string_view fen = "r3k2r/8/8/8/8/8/8/4K3 b kq - 0 1";
    require_round_trip(
        position_from(fen),
        blaze::Move{
            blaze::Square::E8,
            blaze::Square::G8,
            blaze::MoveFlag::CastleKing});
    require_round_trip(
        position_from(fen),
        blaze::Move{
            blaze::Square::E8,
            blaze::Square::C8,
            blaze::MoveFlag::CastleQueen});
}

TEST_CASE(all_promotion_types_round_trip) {
    constexpr std::array<blaze::PieceType, 4> promotions{
        blaze::PieceType::Knight,
        blaze::PieceType::Bishop,
        blaze::PieceType::Rook,
        blaze::PieceType::Queen};

    for (const blaze::PieceType promotion : promotions) {
        require_round_trip(
            position_from("4k3/P7/8/8/8/8/8/4K3 w - - 0 1"),
            blaze::Move{
                blaze::Square::A7,
                blaze::Square::A8,
                blaze::MoveFlag::Promotion,
                promotion});
    }
}

TEST_CASE(null_move_round_trips) {
    auto position = position_from("4k3/8/8/8/4P3/8/8/4K3 b - e3 0 1");
    const std::string before = position.to_fen();
    const std::uint64_t key = position.key();
    blaze::StateInfo state;

    position.make_null(state);
    CHECK_EQ(position.side_to_move(), blaze::Color::White);
    CHECK_EQ(position.ep_square(), blaze::Square::None);
    CHECK(position.key() != key);
    position.unmake_null(state);

    CHECK_EQ(position.to_fen(), before);
    CHECK_EQ(position.key(), key);
}

TEST_CASE(transposed_move_orders_produce_identical_keys) {
    constexpr std::string_view fen =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    auto first = position_from(fen);
    auto second = position_from(fen);
    constexpr std::array<blaze::Move, 4> first_order{
        blaze::Move{blaze::Square::G1, blaze::Square::F3},
        blaze::Move{blaze::Square::G8, blaze::Square::F6},
        blaze::Move{blaze::Square::B1, blaze::Square::C3},
        blaze::Move{blaze::Square::B8, blaze::Square::C6}};
    constexpr std::array<blaze::Move, 4> second_order{
        blaze::Move{blaze::Square::B1, blaze::Square::C3},
        blaze::Move{blaze::Square::B8, blaze::Square::C6},
        blaze::Move{blaze::Square::G1, blaze::Square::F3},
        blaze::Move{blaze::Square::G8, blaze::Square::F6}};

    for (const blaze::Move move : first_order) {
        blaze::StateInfo state;
        CHECK(first.make_move(move, state));
    }
    for (const blaze::Move move : second_order) {
        blaze::StateInfo state;
        CHECK(second.make_move(move, state));
    }

    CHECK_EQ(first.to_fen(), second.to_fen());
    CHECK_EQ(first.key(), second.key());
}

TEST_CASE(non_pawn_double_push_is_rejected_without_state_change) {
    auto position = position_from(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    const std::string before = position.to_fen();
    const std::uint64_t key = position.key();
    blaze::StateInfo state;

    CHECK(!position.make_move(
        blaze::Move{
            blaze::Square::B1,
            blaze::Square::B3,
            blaze::MoveFlag::DoublePush},
        state));
    CHECK_EQ(position.to_fen(), before);
    CHECK_EQ(position.key(), key);
}

TEST_CASE(castling_without_rook_is_rejected_without_state_change) {
    auto position = position_from("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    const std::string before = position.to_fen();
    const std::uint64_t key = position.key();
    blaze::StateInfo state;

    CHECK(!position.make_move(
        blaze::Move{
            blaze::Square::E1,
            blaze::Square::G1,
            blaze::MoveFlag::CastleKing},
        state));
    CHECK_EQ(position.to_fen(), before);
    CHECK_EQ(position.key(), key);
}
