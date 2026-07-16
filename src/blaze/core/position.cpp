#include "blaze/core/position.h"

#include "blaze/core/attacks.h"
#include "blaze/core/move.h"
#include "blaze/core/zobrist.h"

#include <cassert>
#include <bit>
#include <charconv>
#include <sstream>
#include <string>

namespace blaze {

namespace {

[[nodiscard]] Piece piece_from_fen_char(char character) {
    switch (character) {
        case 'P': return Piece::WhitePawn;
        case 'N': return Piece::WhiteKnight;
        case 'B': return Piece::WhiteBishop;
        case 'R': return Piece::WhiteRook;
        case 'Q': return Piece::WhiteQueen;
        case 'K': return Piece::WhiteKing;
        case 'p': return Piece::BlackPawn;
        case 'n': return Piece::BlackKnight;
        case 'b': return Piece::BlackBishop;
        case 'r': return Piece::BlackRook;
        case 'q': return Piece::BlackQueen;
        case 'k': return Piece::BlackKing;
        default: return Piece::None;
    }
}

[[nodiscard]] char fen_char_from_piece(Piece piece) {
    switch (piece) {
        case Piece::WhitePawn: return 'P';
        case Piece::WhiteKnight: return 'N';
        case Piece::WhiteBishop: return 'B';
        case Piece::WhiteRook: return 'R';
        case Piece::WhiteQueen: return 'Q';
        case Piece::WhiteKing: return 'K';
        case Piece::BlackPawn: return 'p';
        case Piece::BlackKnight: return 'n';
        case Piece::BlackBishop: return 'b';
        case Piece::BlackRook: return 'r';
        case Piece::BlackQueen: return 'q';
        case Piece::BlackKing: return 'k';
        case Piece::None: return ' ';
    }
    return ' ';
}

[[nodiscard]] bool parse_nonnegative_int(std::string_view text, int& value) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end && value >= 0;
}

[[nodiscard]] bool castling_pieces_are_present(const Position& position) {
    const CastlingRights rights = position.castling_rights();
    if (has_castling(rights, CastlingRight::WhiteKing) &&
        (position.piece_on(Square::E1) != Piece::WhiteKing ||
         position.piece_on(Square::H1) != Piece::WhiteRook)) {
        return false;
    }
    if (has_castling(rights, CastlingRight::WhiteQueen) &&
        (position.piece_on(Square::E1) != Piece::WhiteKing ||
         position.piece_on(Square::A1) != Piece::WhiteRook)) {
        return false;
    }
    if (has_castling(rights, CastlingRight::BlackKing) &&
        (position.piece_on(Square::E8) != Piece::BlackKing ||
         position.piece_on(Square::H8) != Piece::BlackRook)) {
        return false;
    }
    if (has_castling(rights, CastlingRight::BlackQueen) &&
        (position.piece_on(Square::E8) != Piece::BlackKing ||
         position.piece_on(Square::A8) != Piece::BlackRook)) {
        return false;
    }
    return true;
}

}  // namespace

bool Position::put_piece(Piece piece, Square square) {
    if (piece == Piece::None || !is_valid_square(square) ||
        board_[square_index(square)] != Piece::None) {
        return false;
    }

    const Bitboard bit = square_bit(square);
    board_[square_index(square)] = piece;
    piece_bitboards_[static_cast<std::size_t>(piece_index(piece))] |= bit;
    occupied_ |= bit;
    return true;
}

void Position::add_piece(Piece piece, Square square) {
    assert(piece != Piece::None);
    assert(is_valid_square(square));
    assert(board_[square_index(square)] == Piece::None);
    const Bitboard bit = square_bit(square);
    board_[square_index(square)] = piece;
    piece_bitboards_[static_cast<std::size_t>(piece_index(piece))] |= bit;
    occupied_ |= bit;
    key_ ^= Zobrist::piece(piece, square);
}

Piece Position::remove_piece(Square square) {
    assert(is_valid_square(square));
    const Piece piece = board_[square_index(square)];
    assert(piece != Piece::None);
    const Bitboard bit = square_bit(square);
    board_[square_index(square)] = Piece::None;
    piece_bitboards_[static_cast<std::size_t>(piece_index(piece))] &= ~bit;
    occupied_ &= ~bit;
    key_ ^= Zobrist::piece(piece, square);
    return piece;
}

void Position::relocate_piece(Square from, Square to) {
    const Piece piece = remove_piece(from);
    add_piece(piece, to);
}

bool Position::ep_is_effective() const {
    if (ep_square_ == Square::None) {
        return false;
    }
    const Bitboard possible_attackers = Attacks::pawn(opposite(side_to_move_), ep_square_);
    return (possible_attackers & pieces(side_to_move_, PieceType::Pawn)) != 0;
}

std::uint64_t Position::recompute_key() const {
    std::uint64_t result = Zobrist::castling(castling_rights_);
    for (int index = 0; index < 64; ++index) {
        const Piece piece = board_[index];
        if (piece != Piece::None) {
            result ^= Zobrist::piece(piece, static_cast<Square>(index));
        }
    }
    if (side_to_move_ == Color::Black) {
        result ^= Zobrist::side();
    }
    if (ep_is_effective()) {
        result ^= Zobrist::ep_file(file_of(ep_square_));
    }
    return result;
}

void Position::update_castling_rights(
    Piece mover,
    Square from,
    Piece captured,
    Square captured_square) {
    const auto clear = [this](CastlingRight right) {
        castling_rights_ &= static_cast<CastlingRights>(
            ~static_cast<CastlingRights>(right));
    };

    if (mover == Piece::WhiteKing) {
        clear(CastlingRight::WhiteKing);
        clear(CastlingRight::WhiteQueen);
    } else if (mover == Piece::BlackKing) {
        clear(CastlingRight::BlackKing);
        clear(CastlingRight::BlackQueen);
    } else if (mover == Piece::WhiteRook) {
        if (from == Square::H1) clear(CastlingRight::WhiteKing);
        if (from == Square::A1) clear(CastlingRight::WhiteQueen);
    } else if (mover == Piece::BlackRook) {
        if (from == Square::H8) clear(CastlingRight::BlackKing);
        if (from == Square::A8) clear(CastlingRight::BlackQueen);
    }

    if (captured == Piece::WhiteRook) {
        if (captured_square == Square::H1) clear(CastlingRight::WhiteKing);
        if (captured_square == Square::A1) clear(CastlingRight::WhiteQueen);
    } else if (captured == Piece::BlackRook) {
        if (captured_square == Square::H8) clear(CastlingRight::BlackKing);
        if (captured_square == Square::A8) clear(CastlingRight::BlackQueen);
    }
}

std::optional<Position> Position::from_fen(std::string_view fen) {
    std::istringstream stream{std::string(fen)};
    std::string board_text;
    std::string side_text;
    std::string castling_text;
    std::string ep_text;
    std::string rule50_text;
    std::string fullmove_text;
    std::string extra;
    if (!(stream >> board_text >> side_text >> castling_text >> ep_text >>
          rule50_text >> fullmove_text) ||
        (stream >> extra)) {
        return std::nullopt;
    }

    Position position;
    int rank = 7;
    int file = 0;
    int rank_count = 1;
    for (const char character : board_text) {
        if (character == '/') {
            if (file != 8 || rank <= 0) {
                return std::nullopt;
            }
            --rank;
            file = 0;
            ++rank_count;
            continue;
        }

        if (character >= '1' && character <= '8') {
            file += character - '0';
            if (file > 8) {
                return std::nullopt;
            }
            continue;
        }

        const Piece piece = piece_from_fen_char(character);
        const bool pawn_on_back_rank =
            (piece == Piece::WhitePawn || piece == Piece::BlackPawn) &&
            (rank == 0 || rank == 7);
        if (piece == Piece::None || pawn_on_back_rank || file >= 8 ||
            !position.put_piece(piece, make_square(file, rank))) {
            return std::nullopt;
        }
        ++file;
    }
    if (rank_count != 8 || rank != 0 || file != 8) {
        return std::nullopt;
    }

    if (side_text == "w") {
        position.side_to_move_ = Color::White;
    } else if (side_text == "b") {
        position.side_to_move_ = Color::Black;
    } else {
        return std::nullopt;
    }

    if (castling_text != "-") {
        for (const char right : castling_text) {
            CastlingRight parsed = CastlingRight::WhiteKing;
            switch (right) {
                case 'K': parsed = CastlingRight::WhiteKing; break;
                case 'Q': parsed = CastlingRight::WhiteQueen; break;
                case 'k': parsed = CastlingRight::BlackKing; break;
                case 'q': parsed = CastlingRight::BlackQueen; break;
                default: return std::nullopt;
            }
            const CastlingRights bit = static_cast<CastlingRights>(parsed);
            if ((position.castling_rights_ & bit) != 0U) {
                return std::nullopt;
            }
            position.castling_rights_ |= bit;
        }
    }

    if (ep_text != "-") {
        position.ep_square_ = square_from_string(ep_text);
        if (!is_valid_square(position.ep_square_)) {
            return std::nullopt;
        }
        const bool correct_rank =
            (position.side_to_move_ == Color::Black && rank_of(position.ep_square_) == 2) ||
            (position.side_to_move_ == Color::White && rank_of(position.ep_square_) == 5);
        const int pawn_offset = position.side_to_move_ == Color::Black ? 8 : -8;
        const Square pawn_square = static_cast<Square>(
            square_index(position.ep_square_) + pawn_offset);
        const Piece expected_pawn = position.side_to_move_ == Color::Black
            ? Piece::WhitePawn
            : Piece::BlackPawn;
        if (!correct_rank || position.piece_on(pawn_square) != expected_pawn) {
            return std::nullopt;
        }
    }

    if (!parse_nonnegative_int(rule50_text, position.rule50_) ||
        !parse_nonnegative_int(fullmove_text, position.fullmove_number_) ||
        position.fullmove_number_ < 1) {
        return std::nullopt;
    }

    const Bitboard white_king = position.pieces(Color::White, PieceType::King);
    const Bitboard black_king = position.pieces(Color::Black, PieceType::King);
    if (std::popcount(white_king) != 1 || std::popcount(black_king) != 1) {
        return std::nullopt;
    }
    const Square white_king_square = static_cast<Square>(std::countr_zero(white_king));
    if ((Attacks::king(white_king_square) & black_king) != 0) {
        return std::nullopt;
    }
    position.key_ = position.recompute_key();
    if (!castling_pieces_are_present(position) || !position.is_consistent()) {
        return std::nullopt;
    }
    return position;
}

std::string Position::to_fen() const {
    std::string fen;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            const Piece piece = piece_on(make_square(file, rank));
            if (piece == Piece::None) {
                ++empty;
                continue;
            }
            if (empty != 0) {
                fen.push_back(static_cast<char>('0' + empty));
                empty = 0;
            }
            fen.push_back(fen_char_from_piece(piece));
        }
        if (empty != 0) {
            fen.push_back(static_cast<char>('0' + empty));
        }
        if (rank != 0) {
            fen.push_back('/');
        }
    }

    fen += side_to_move_ == Color::White ? " w " : " b ";
    if (castling_rights_ == 0U) {
        fen.push_back('-');
    } else {
        if (has_castling(castling_rights_, CastlingRight::WhiteKing)) fen.push_back('K');
        if (has_castling(castling_rights_, CastlingRight::WhiteQueen)) fen.push_back('Q');
        if (has_castling(castling_rights_, CastlingRight::BlackKing)) fen.push_back('k');
        if (has_castling(castling_rights_, CastlingRight::BlackQueen)) fen.push_back('q');
    }
    fen.push_back(' ');
    fen += ep_square_ == Square::None ? "-" : square_to_string(ep_square_);
    fen.push_back(' ');
    fen += std::to_string(rule50_);
    fen.push_back(' ');
    fen += std::to_string(fullmove_number_);
    return fen;
}

Piece Position::piece_on(Square square) const {
    return is_valid_square(square) ? board_[square_index(square)] : Piece::None;
}

Bitboard Position::pieces(Color color, PieceType type) const {
    if (type == PieceType::None) {
        return 0;
    }
    return piece_bitboards_[static_cast<std::size_t>(
        piece_index(make_piece(color, type)))];
}

bool Position::is_consistent() const {
    std::array<Bitboard, 12> rebuilt{};
    Bitboard occupied = 0;
    for (int index = 0; index < 64; ++index) {
        const Piece piece = board_[index];
        if (piece == Piece::None) {
            continue;
        }
        const Bitboard bit = Bitboard{1} << static_cast<unsigned>(index);
        rebuilt[static_cast<std::size_t>(piece_index(piece))] |= bit;
        occupied |= bit;
    }
    return rebuilt == piece_bitboards_ && occupied == occupied_ && key_ == recompute_key();
}

bool Position::make_move(Move move, StateInfo& state) {
    if (!move.is_valid()) {
        return false;
    }
    const Piece mover = piece_on(move.from());
    if (mover == Piece::None || color_of(mover) != side_to_move_) {
        return false;
    }

    const bool is_en_passant = move.has_flag(MoveFlag::EnPassant);
    const bool is_capture = move.has_flag(MoveFlag::Capture);
    const bool is_promotion = move.has_flag(MoveFlag::Promotion);
    const bool is_castle = move.has_flag(MoveFlag::CastleKing) ||
                           move.has_flag(MoveFlag::CastleQueen);

    if (is_castle) {
        const bool king_side = move.has_flag(MoveFlag::CastleKing);
        const Square expected_from =
            side_to_move_ == Color::White ? Square::E1 : Square::E8;
        const Square expected_to = side_to_move_ == Color::White
            ? (king_side ? Square::G1 : Square::C1)
            : (king_side ? Square::G8 : Square::C8);
        const Square rook_square = side_to_move_ == Color::White
            ? (king_side ? Square::H1 : Square::A1)
            : (king_side ? Square::H8 : Square::A8);
        const CastlingRight required_right = side_to_move_ == Color::White
            ? (king_side ? CastlingRight::WhiteKing : CastlingRight::WhiteQueen)
            : (king_side ? CastlingRight::BlackKing : CastlingRight::BlackQueen);
        if (move.from() != expected_from || move.to() != expected_to ||
            piece_on(move.to()) != Piece::None ||
            piece_on(rook_square) != make_piece(side_to_move_, PieceType::Rook) ||
            !has_castling(castling_rights_, required_right) ||
            is_capture || is_en_passant || is_promotion ||
            move.has_flag(MoveFlag::DoublePush)) {
            return false;
        }
    }

    Square captured_square = move.to();
    if (is_en_passant) {
        captured_square = static_cast<Square>(
            square_index(move.to()) + (side_to_move_ == Color::White ? -8 : 8));
    }
    const Piece captured = is_capture ? piece_on(captured_square) : Piece::None;

    if ((is_capture && captured == Piece::None) ||
        (!is_capture && !is_castle && piece_on(move.to()) != Piece::None) ||
        (captured != Piece::None && color_of(captured) == side_to_move_) ||
        (is_promotion && type_of(mover) != PieceType::Pawn) ||
        (move.has_flag(MoveFlag::DoublePush) && type_of(mover) != PieceType::Pawn) ||
        (is_castle && type_of(mover) != PieceType::King)) {
        return false;
    }

    state.side_to_move = side_to_move_;
    state.castling_rights = castling_rights_;
    state.ep_square = ep_square_;
    state.rule50 = rule50_;
    state.fullmove_number = fullmove_number_;
    state.key = key_;
    state.captured_piece = captured;
    state.captured_square = captured == Piece::None ? Square::None : captured_square;

    if (ep_is_effective()) {
        key_ ^= Zobrist::ep_file(file_of(ep_square_));
    }
    key_ ^= Zobrist::castling(castling_rights_);
    ep_square_ = Square::None;

    if (captured != Piece::None) {
        remove_piece(captured_square);
    }

    if (move.has_flag(MoveFlag::CastleKing)) {
        relocate_piece(move.from(), move.to());
        const Square rook_from = side_to_move_ == Color::White ? Square::H1 : Square::H8;
        const Square rook_to = side_to_move_ == Color::White ? Square::F1 : Square::F8;
        relocate_piece(rook_from, rook_to);
    } else if (move.has_flag(MoveFlag::CastleQueen)) {
        relocate_piece(move.from(), move.to());
        const Square rook_from = side_to_move_ == Color::White ? Square::A1 : Square::A8;
        const Square rook_to = side_to_move_ == Color::White ? Square::D1 : Square::D8;
        relocate_piece(rook_from, rook_to);
    } else if (is_promotion) {
        remove_piece(move.from());
        add_piece(make_piece(side_to_move_, move.promotion()), move.to());
    } else {
        relocate_piece(move.from(), move.to());
    }

    update_castling_rights(mover, move.from(), captured, captured_square);
    if (move.has_flag(MoveFlag::DoublePush)) {
        ep_square_ = static_cast<Square>(
            (square_index(move.from()) + square_index(move.to())) / 2);
    }

    rule50_ = type_of(mover) == PieceType::Pawn || captured != Piece::None
        ? 0
        : rule50_ + 1;
    if (side_to_move_ == Color::Black) {
        ++fullmove_number_;
    }
    side_to_move_ = opposite(side_to_move_);
    key_ ^= Zobrist::side();
    key_ ^= Zobrist::castling(castling_rights_);
    if (ep_is_effective()) {
        key_ ^= Zobrist::ep_file(file_of(ep_square_));
    }

    assert(is_consistent());
    return true;
}

void Position::unmake_move(Move move, const StateInfo& state) {
    side_to_move_ = state.side_to_move;

    if (move.has_flag(MoveFlag::CastleKing)) {
        relocate_piece(move.to(), move.from());
        const Square rook_from = side_to_move_ == Color::White ? Square::F1 : Square::F8;
        const Square rook_to = side_to_move_ == Color::White ? Square::H1 : Square::H8;
        relocate_piece(rook_from, rook_to);
    } else if (move.has_flag(MoveFlag::CastleQueen)) {
        relocate_piece(move.to(), move.from());
        const Square rook_from = side_to_move_ == Color::White ? Square::D1 : Square::D8;
        const Square rook_to = side_to_move_ == Color::White ? Square::A1 : Square::A8;
        relocate_piece(rook_from, rook_to);
    } else if (move.has_flag(MoveFlag::Promotion)) {
        remove_piece(move.to());
        add_piece(make_piece(side_to_move_, PieceType::Pawn), move.from());
    } else {
        relocate_piece(move.to(), move.from());
    }

    if (state.captured_piece != Piece::None) {
        add_piece(state.captured_piece, state.captured_square);
    }

    castling_rights_ = state.castling_rights;
    ep_square_ = state.ep_square;
    rule50_ = state.rule50;
    fullmove_number_ = state.fullmove_number;
    key_ = state.key;
    assert(is_consistent());
}

void Position::make_null(StateInfo& state) {
    state.side_to_move = side_to_move_;
    state.castling_rights = castling_rights_;
    state.ep_square = ep_square_;
    state.rule50 = rule50_;
    state.fullmove_number = fullmove_number_;
    state.key = key_;
    state.captured_piece = Piece::None;
    state.captured_square = Square::None;

    if (ep_is_effective()) {
        key_ ^= Zobrist::ep_file(file_of(ep_square_));
    }
    ep_square_ = Square::None;
    ++rule50_;
    if (side_to_move_ == Color::Black) {
        ++fullmove_number_;
    }
    side_to_move_ = opposite(side_to_move_);
    key_ ^= Zobrist::side();
    assert(is_consistent());
}

void Position::unmake_null(const StateInfo& state) {
    side_to_move_ = state.side_to_move;
    castling_rights_ = state.castling_rights;
    ep_square_ = state.ep_square;
    rule50_ = state.rule50;
    fullmove_number_ = state.fullmove_number;
    key_ = state.key;
    assert(is_consistent());
}

}  // namespace blaze
