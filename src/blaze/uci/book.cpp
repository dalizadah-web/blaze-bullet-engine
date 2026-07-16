#include "blaze/uci/book.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace blaze {
namespace {

std::uint16_t read_be16(const std::array<std::uint8_t, 16>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((bytes[offset] << 8U) | bytes[offset + 1]);
}

std::uint64_t read_be64(const std::array<std::uint8_t, 16>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

Move decode_move(std::uint16_t encoded, const MoveList& legal_moves) {
    Square from = static_cast<Square>(encoded & 0x3FU);
    Square to = static_cast<Square>((encoded >> 6U) & 0x3FU);
    const int promotion = (encoded >> 12U) & 7U;
    if ((from == Square::E1 && to == Square::H1) ||
        (from == Square::E8 && to == Square::H8)) {
        to = static_cast<Square>(square_index(from) + 2);
    } else if ((from == Square::E1 && to == Square::A1) ||
               (from == Square::E8 && to == Square::A8)) {
        to = static_cast<Square>(square_index(from) - 2);
    }
    PieceType promotion_type = PieceType::None;
    switch (promotion) {
        case 1: promotion_type = PieceType::Knight; break;
        case 2: promotion_type = PieceType::Bishop; break;
        case 3: promotion_type = PieceType::Rook; break;
        case 4: promotion_type = PieceType::Queen; break;
        default: break;
    }
    for (const Move move : legal_moves) {
        if (move.from() == from && move.to() == to && move.promotion() == promotion_type) {
            return move;
        }
    }
    return Move{};
}

std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 7U;
    state ^= state >> 9U;
    state ^= state << 8U;
    return state;
}

}  // namespace

std::optional<PolyglotBook> PolyglotBook::load(
    std::string_view path,
    std::string& error) {
    error.clear();
    std::ifstream input(std::string(path), std::ios::binary);
    if (!input) {
        error = "book file cannot be opened";
        return std::nullopt;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size < 0 || size % 16 != 0 || size > 512 * 1024 * 1024) {
        error = "book file is not a sequence of 16-byte records";
        return std::nullopt;
    }

    PolyglotBook book;
    const std::size_t count = static_cast<std::size_t>(size / 16);
    book.entries_.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::array<std::uint8_t, 16> record{};
        input.read(reinterpret_cast<char*>(record.data()), record.size());
        if (!input) {
            error = "book file is truncated";
            return std::nullopt;
        }
        book.entries_.push_back(Entry{
            read_be64(record, 0),
            read_be16(record, 8),
            read_be16(record, 10)});
    }
    std::stable_sort(book.entries_.begin(), book.entries_.end(), [](const Entry& left, const Entry& right) {
        return left.key < right.key;
    });
    return book;
}

std::optional<Move> PolyglotBook::select(
    std::uint64_t key,
    const Position& position,
    const MoveList& legal_moves,
    BookPolicy policy,
    std::uint64_t seed) const {
    static_cast<void>(position);
    std::vector<std::pair<Move, std::uint16_t>> candidates;
    for (const Entry& entry : entries_) {
        if (entry.key < key) continue;
        if (entry.key > key) break;
        const Move move = decode_move(entry.move, legal_moves);
        if (move.is_valid()) {
            candidates.emplace_back(move, entry.weight);
        }
    }
    if (candidates.empty()) return std::nullopt;
    if (policy == BookPolicy::Best) {
        return std::max_element(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
            return left.second < right.second;
        })->first;
    }
    std::uint64_t total = 0;
    for (const auto& candidate : candidates) total += candidate.second;
    if (total == 0) return candidates.front().first;
    const std::uint64_t target = next_random(seed) % total;
    std::uint64_t cumulative = 0;
    for (const auto& candidate : candidates) {
        cumulative += candidate.second;
        if (target < cumulative) return candidate.first;
    }
    return candidates.back().first;
}

}  // namespace blaze
