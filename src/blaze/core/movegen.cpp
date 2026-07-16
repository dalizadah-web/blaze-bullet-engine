#include "blaze/core/movegen.h"

#include "blaze/core/attacks.h"

#include <bit>

namespace blaze {

namespace {

[[nodiscard]] Square pop_first(Bitboard& board) {
    const Square square = static_cast<Square>(std::countr_zero(board));
    board &= board - 1;
    return square;
}

[[nodiscard]] Bitboard color_occupancy(const Position& position, Color color) {
    return position.pieces(color, PieceType::Pawn) |
           position.pieces(color, PieceType::Knight) |
           position.pieces(color, PieceType::Bishop) |
           position.pieces(color, PieceType::Rook) |
           position.pieces(color, PieceType::Queen) |
           position.pieces(color, PieceType::King);
}

void add_promotions(MoveList& moves, Square from, Square to, MoveFlag base_flags) {
    constexpr PieceType promotion_types[]{
        PieceType::Knight,
        PieceType::Bishop,
        PieceType::Rook,
        PieceType::Queen};
    for (const PieceType promotion : promotion_types) {
        moves.push(Move{from, to, base_flags | MoveFlag::Promotion, promotion});
    }
}

void add_piece_moves(
    const Position& position,
    MoveList& moves,
    Bitboard pieces,
    Bitboard own,
    Bitboard enemy,
    PieceType type,
    GenType generation_type) {
    while (pieces != 0) {
        const Square from = pop_first(pieces);
        Bitboard targets = 0;
        switch (type) {
            case PieceType::Knight: targets = Attacks::knight(from); break;
            case PieceType::Bishop: targets = Attacks::bishop(from, position.occupied()); break;
            case PieceType::Rook: targets = Attacks::rook(from, position.occupied()); break;
            case PieceType::Queen: targets = Attacks::queen(from, position.occupied()); break;
            case PieceType::King: targets = Attacks::king(from); break;
            case PieceType::None:
            case PieceType::Pawn: break;
        }
        targets &= ~own;
        if (generation_type == GenType::Captures) {
            targets &= enemy;
        } else if (generation_type == GenType::Quiets) {
            targets &= ~enemy;
        }
        while (targets != 0) {
            const Square to = pop_first(targets);
            const Piece target_piece = position.piece_on(to);
            if (type_of(target_piece) == PieceType::King) {
                continue;
            }
            const MoveFlag flag = (enemy & square_bit(to)) != 0
                ? MoveFlag::Capture
                : MoveFlag::Quiet;
            moves.push(Move{from, to, flag});
        }
    }
}

void add_pawn_moves(
    const Position& position,
    MoveList& moves,
    Bitboard enemy,
    GenType generation_type) {
    const Color side = position.side_to_move();
    Bitboard pawns = position.pieces(side, PieceType::Pawn);
    const int step = side == Color::White ? 8 : -8;
    const int start_rank = side == Color::White ? 1 : 6;
    const int promotion_rank = side == Color::White ? 6 : 1;

    while (pawns != 0) {
        const Square from = pop_first(pawns);
        const int forward_index = square_index(from) + step;
        if (forward_index >= 0 && forward_index < 64) {
            const Square forward = static_cast<Square>(forward_index);
            if (position.piece_on(forward) == Piece::None) {
                if (rank_of(from) == promotion_rank) {
                    if (generation_type != GenType::Quiets) {
                        add_promotions(moves, from, forward, MoveFlag::Quiet);
                    }
                } else if (generation_type != GenType::Captures) {
                    moves.push(Move{from, forward});
                    if (rank_of(from) == start_rank) {
                        const Square double_to = static_cast<Square>(forward_index + step);
                        if (position.piece_on(double_to) == Piece::None) {
                            moves.push(Move{from, double_to, MoveFlag::DoublePush});
                        }
                    }
                }
            }
        }

        if (generation_type != GenType::Quiets) {
            Bitboard captures = Attacks::pawn(side, from) & enemy;
            while (captures != 0) {
                const Square to = pop_first(captures);
                if (type_of(position.piece_on(to)) == PieceType::King) {
                    continue;
                }
                if (rank_of(from) == promotion_rank) {
                    add_promotions(moves, from, to, MoveFlag::Capture);
                } else {
                    moves.push(Move{from, to, MoveFlag::Capture});
                }
            }

            if (position.ep_square() != Square::None &&
                (Attacks::pawn(side, from) & square_bit(position.ep_square())) != 0) {
                moves.push(Move{
                    from,
                    position.ep_square(),
                    MoveFlag::Capture | MoveFlag::EnPassant});
            }
        }
    }
}

void add_castles(const Position& position, MoveList& moves) {
    const Color side = position.side_to_move();
    const Color enemy = opposite(side);
    const CastlingRights rights = position.castling_rights();
    const Square king_square = side == Color::White ? Square::E1 : Square::E8;
    if (is_square_attacked(position, king_square, enemy)) {
        return;
    }

    if (side == Color::White) {
        if (has_castling(rights, CastlingRight::WhiteKing) &&
            position.piece_on(Square::F1) == Piece::None &&
            position.piece_on(Square::G1) == Piece::None &&
            !is_square_attacked(position, Square::F1, enemy) &&
            !is_square_attacked(position, Square::G1, enemy)) {
            moves.push(Move{Square::E1, Square::G1, MoveFlag::CastleKing});
        }
        if (has_castling(rights, CastlingRight::WhiteQueen) &&
            position.piece_on(Square::D1) == Piece::None &&
            position.piece_on(Square::C1) == Piece::None &&
            position.piece_on(Square::B1) == Piece::None &&
            !is_square_attacked(position, Square::D1, enemy) &&
            !is_square_attacked(position, Square::C1, enemy)) {
            moves.push(Move{Square::E1, Square::C1, MoveFlag::CastleQueen});
        }
    } else {
        if (has_castling(rights, CastlingRight::BlackKing) &&
            position.piece_on(Square::F8) == Piece::None &&
            position.piece_on(Square::G8) == Piece::None &&
            !is_square_attacked(position, Square::F8, enemy) &&
            !is_square_attacked(position, Square::G8, enemy)) {
            moves.push(Move{Square::E8, Square::G8, MoveFlag::CastleKing});
        }
        if (has_castling(rights, CastlingRight::BlackQueen) &&
            position.piece_on(Square::D8) == Piece::None &&
            position.piece_on(Square::C8) == Piece::None &&
            position.piece_on(Square::B8) == Piece::None &&
            !is_square_attacked(position, Square::D8, enemy) &&
            !is_square_attacked(position, Square::C8, enemy)) {
            moves.push(Move{Square::E8, Square::C8, MoveFlag::CastleQueen});
        }
    }
}

}  // namespace

bool is_square_attacked(const Position& position, Square square, Color by_color) {
    if ((Attacks::pawn(opposite(by_color), square) &
         position.pieces(by_color, PieceType::Pawn)) != 0) {
        return true;
    }
    if ((Attacks::knight(square) & position.pieces(by_color, PieceType::Knight)) != 0) {
        return true;
    }
    if ((Attacks::king(square) & position.pieces(by_color, PieceType::King)) != 0) {
        return true;
    }
    const Bitboard diagonal = position.pieces(by_color, PieceType::Bishop) |
                              position.pieces(by_color, PieceType::Queen);
    if ((Attacks::bishop(square, position.occupied()) & diagonal) != 0) {
        return true;
    }
    const Bitboard straight = position.pieces(by_color, PieceType::Rook) |
                              position.pieces(by_color, PieceType::Queen);
    return (Attacks::rook(square, position.occupied()) & straight) != 0;
}

bool in_check(const Position& position) {
    const Bitboard king = position.pieces(position.side_to_move(), PieceType::King);
    const Square king_square = static_cast<Square>(std::countr_zero(king));
    return is_square_attacked(position, king_square, opposite(position.side_to_move()));
}

void generate_pseudo_legal(const Position& position, MoveList& moves) {
    generate_pseudo_legal(position, moves, GenType::All);
}

void generate_pseudo_legal(const Position& position, MoveList& moves, GenType type) {
    moves.clear();
    const Color side = position.side_to_move();
    const Color enemy_side = opposite(side);
    const Bitboard own = color_occupancy(position, side);
    const Bitboard enemy = color_occupancy(position, enemy_side);

    add_pawn_moves(position, moves, enemy, type);
    add_piece_moves(position, moves, position.pieces(side, PieceType::Knight), own, enemy, PieceType::Knight, type);
    add_piece_moves(position, moves, position.pieces(side, PieceType::Bishop), own, enemy, PieceType::Bishop, type);
    add_piece_moves(position, moves, position.pieces(side, PieceType::Rook), own, enemy, PieceType::Rook, type);
    add_piece_moves(position, moves, position.pieces(side, PieceType::Queen), own, enemy, PieceType::Queen, type);
    add_piece_moves(position, moves, position.pieces(side, PieceType::King), own, enemy, PieceType::King, type);
    if (type != GenType::Captures) add_castles(position, moves);
}

void generate_legal(Position& position, MoveList& moves) {
    MoveList candidates;
    generate_pseudo_legal(position, candidates);
    moves.clear();
    const Color moving_side = position.side_to_move();

    for (const Move move : candidates) {
        StateInfo state;
        if (!position.make_move(move, state)) {
            continue;
        }
        const Bitboard king = position.pieces(moving_side, PieceType::King);
        const Square king_square = static_cast<Square>(std::countr_zero(king));
        const bool legal = !is_square_attacked(position, king_square, opposite(moving_side));
        position.unmake_move(move, state);
        if (legal) {
            moves.push(move);
        }
    }
}

bool Position::is_legal(Move move) {
    MoveList legal_moves;
    generate_legal(*this, legal_moves);
    for (const Move candidate : legal_moves) {
        if (candidate.from() == move.from() && candidate.to() == move.to() &&
            candidate.promotion() == move.promotion()) {
            return true;
        }
    }
    return false;
}

}  // namespace blaze
