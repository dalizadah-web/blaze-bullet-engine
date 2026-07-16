#include "blaze/core/move.h"

namespace blaze {

Square square_from_string(std::string_view text) {
    if (text.size() != 2U || text[0] < 'a' || text[0] > 'h' ||
        text[1] < '1' || text[1] > '8') {
        return Square::None;
    }
    return make_square(text[0] - 'a', text[1] - '1');
}

std::string square_to_string(Square square) {
    if (!is_valid_square(square)) {
        return "--";
    }

    std::string text(2, ' ');
    text[0] = static_cast<char>('a' + file_of(square));
    text[1] = static_cast<char>('1' + rank_of(square));
    return text;
}

std::optional<Move> move_from_uci(std::string_view text) {
    if (text.size() != 4U && text.size() != 5U) {
        return std::nullopt;
    }

    const Square from = square_from_string(text.substr(0, 2));
    const Square to = square_from_string(text.substr(2, 2));
    if (!is_valid_square(from) || !is_valid_square(to) || from == to) {
        return std::nullopt;
    }

    MoveFlag flags = MoveFlag::Quiet;
    PieceType promotion = PieceType::None;
    if (text.size() == 5U) {
        const bool reaches_back_rank =
            (rank_of(from) == 6 && rank_of(to) == 7) ||
            (rank_of(from) == 1 && rank_of(to) == 0);
        if (!reaches_back_rank) {
            return std::nullopt;
        }

        switch (text[4]) {
            case 'n': promotion = PieceType::Knight; break;
            case 'b': promotion = PieceType::Bishop; break;
            case 'r': promotion = PieceType::Rook; break;
            case 'q': promotion = PieceType::Queen; break;
            default: return std::nullopt;
        }
        flags = MoveFlag::Promotion;
    }

    const Move move{from, to, flags, promotion};
    return move.is_valid() ? std::optional<Move>{move} : std::nullopt;
}

std::string move_to_uci(Move move) {
    if (!move.is_valid()) {
        return "0000";
    }

    std::string text = square_to_string(move.from()) + square_to_string(move.to());
    if (move.has_flag(MoveFlag::Promotion)) {
        switch (move.promotion()) {
            case PieceType::Knight: text.push_back('n'); break;
            case PieceType::Bishop: text.push_back('b'); break;
            case PieceType::Rook: text.push_back('r'); break;
            case PieceType::Queen: text.push_back('q'); break;
            default: return "0000";
        }
    }
    return text;
}

}  // namespace blaze
