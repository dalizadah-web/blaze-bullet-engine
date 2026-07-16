#include "blaze/uci/book.h"

#include "blaze/core/movegen.h"
#include "blaze/core/position.h"
#include "test_support.h"

#include <array>
#include <cstdint>
#include <fstream>

namespace blaze {
namespace {

void write_u64(std::ofstream& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.put(static_cast<char>((value >> shift) & 0xFFU));
    }
}

void write_u16(std::ofstream& output, std::uint16_t value) {
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
    output.put(static_cast<char>(value & 0xFFU));
}

void write_u32(std::ofstream& output, std::uint32_t value) {
    output.put(static_cast<char>((value >> 24U) & 0xFFU));
    output.put(static_cast<char>((value >> 16U) & 0xFFU));
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
    output.put(static_cast<char>(value & 0xFFU));
}

std::uint16_t polyglot_move(Square from, Square to, int promotion = 0) {
    return static_cast<std::uint16_t>(square_index(from) |
        (square_index(to) << 6U) | (promotion << 12U));
}

TEST_CASE(polyglot_book_rejects_truncated_records) {
    const std::string path = "build/blaze/truncated.bin";
    std::ofstream output(path, std::ios::binary);
    output.put('x');
    output.close();
    std::string error;
    CHECK(!PolyglotBook::load(path, error).has_value());
}

TEST_CASE(polyglot_book_translates_castling_and_filters_legal_moves) {
    const std::string path = "build/blaze/castle.bin";
    std::ofstream output(path, std::ios::binary);
    write_u64(output, 0x1234);
    write_u16(output, polyglot_move(Square::E1, Square::H1));
    write_u16(output, 10);
    write_u32(output, 0);
    output.close();

    std::string error;
    const auto book = PolyglotBook::load(path, error);
    CHECK(book.has_value());
    const auto parsed = Position::from_fen(
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    CHECK(parsed.has_value());
    Position position = *parsed;
    MoveList legal;
    generate_legal(position, legal);
    const auto selected = book->select(0x1234, position, legal, BookPolicy::Best, 1);
    CHECK(selected.has_value());
    CHECK_EQ(move_to_uci(*selected), "e1g1");
}

TEST_CASE(polyglot_book_weighted_selection_is_seeded) {
    const std::string path = "build/blaze/weighted.bin";
    std::ofstream output(path, std::ios::binary);
    write_u64(output, 9);
    write_u16(output, polyglot_move(Square::E2, Square::E4));
    write_u16(output, 1);
    write_u32(output, 0);
    write_u64(output, 9);
    write_u16(output, polyglot_move(Square::D2, Square::D4));
    write_u16(output, 100);
    write_u32(output, 0);
    output.close();

    std::string error;
    const auto book = PolyglotBook::load(path, error);
    CHECK(book.has_value());
    const auto parsed = Position::from_fen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(parsed.has_value());
    Position position = *parsed;
    MoveList legal;
    generate_legal(position, legal);
    const auto first = book->select(9, position, legal, BookPolicy::Weighted, 42);
    const auto second = book->select(9, position, legal, BookPolicy::Weighted, 42);
    CHECK(first.has_value());
    CHECK(second.has_value());
    CHECK_EQ(*first, *second);
}

}  // namespace
}  // namespace blaze
