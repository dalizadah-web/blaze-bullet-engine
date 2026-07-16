#include "blaze/search/search.h"

#include "blaze/core/movegen.h"
#include "blaze/eval/classical.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace blaze {
namespace {

constexpr int infinity = search_mate_score + 1;
constexpr int maximum_ply = 128;

int victim_value(Piece piece) {
    constexpr std::array<int, 7> values = {0, 100, 320, 335, 500, 900, 20000};
    return values[static_cast<std::size_t>(type_of(piece))];
}

int move_order_score(const Position& position, Move move, Move tt_move) {
    if (tt_move.is_valid() && move == tt_move) {
        return 1'000'000;
    }
    int score = 0;
    if (move.has_flag(MoveFlag::Capture) || move.has_flag(MoveFlag::EnPassant)) {
        const Piece victim = move.has_flag(MoveFlag::EnPassant)
            ? make_piece(opposite(position.side_to_move()), PieceType::Pawn)
            : position.piece_on(move.to());
        const Piece attacker = position.piece_on(move.from());
        score += 100'000 + victim_value(victim) * 16 - victim_value(attacker);
    }
    if (move.has_flag(MoveFlag::Promotion)) {
        score += 80'000 + victim_value(make_piece(position.side_to_move(), move.promotion()));
    }
    return score;
}

std::vector<Move> ordered_moves(const Position& position, const MoveList& list, Move tt_move) {
    std::vector<std::pair<int, Move>> scored;
    scored.reserve(list.size());
    for (const Move move : list) {
        scored.emplace_back(move_order_score(position, move, tt_move), move);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& left, const auto& right) {
        return left.first > right.first;
    });
    std::vector<Move> result;
    result.reserve(scored.size());
    for (const auto& [score, move] : scored) {
        static_cast<void>(score);
        result.push_back(move);
    }
    return result;
}

}  // namespace

SearchResult Searcher::search(
    Position position,
    const SearchLimits& limits,
    const std::atomic<bool>* external_stop,
    const std::vector<std::uint64_t>& prior_keys) {
    MoveList legal_moves;
    generate_legal(position, legal_moves);

    SearchResult result;
    if (legal_moves.empty()) {
        result.score = in_check(position) ? -search_mate_score : 0;
        return result;
    }
    result.best_move = legal_moves[0];
    result.pv = {result.best_move};

    if (position.rule50() >= 100) {
        return result;
    }

    Context context;
    context.limits = limits;
    context.external_stop = external_stop;
    context.start = std::chrono::steady_clock::now();
    context.keys = prior_keys;
    context.keys.push_back(position.key());

    if (should_stop(context)) {
        result.stopped = true;
        return result;
    }

    table_.new_search();
    const int maximum_depth = limits.depth > 0 ? limits.depth : 64;
    for (int depth = 1; depth <= maximum_depth; ++depth) {
        std::vector<Move> pv;
        const int score = negamax(position, depth, -infinity, infinity, 0, context, pv);
        if (context.stopped) {
            break;
        }
        if (!pv.empty()) {
            result.best_move = pv.front();
            result.pv = std::move(pv);
        }
        result.score = score;
        result.depth = depth;
        if (score >= search_mate_threshold || score <= -search_mate_threshold) {
            break;
        }
    }

    result.nodes = context.nodes;
    result.stopped = context.stopped;
    return result;
}

int Searcher::negamax(
    Position& position,
    int depth,
    int alpha,
    int beta,
    int ply,
    Context& context,
    std::vector<Move>& pv) {
    pv.clear();
    if (depth <= 0) {
        return quiescence(position, alpha, beta, ply, context, pv);
    }
    if (should_stop(context)) {
        return 0;
    }
    ++context.nodes;

    MoveList legal_moves;
    generate_legal(position, legal_moves);
    if (legal_moves.empty()) {
        return in_check(position) ? -search_mate_score + ply : 0;
    }
    if (position.rule50() >= 100 || is_repetition(context, position.key())) {
        return 0;
    }
    if (ply >= maximum_ply) {
        return evaluate(position);
    }

    Move tt_move;
    const auto tt_hit = table_.probe(position.key(), ply);
    if (tt_hit) {
        tt_move = tt_hit->move;
        if (tt_hit->depth >= depth) {
            if (tt_hit->bound == Bound::Exact ||
                (tt_hit->bound == Bound::Lower && tt_hit->score >= beta) ||
                (tt_hit->bound == Bound::Upper && tt_hit->score <= alpha)) {
                return tt_hit->score;
            }
        }
    }

    const int original_alpha = alpha;
    Move best_move;
    int best_score = -infinity;
    for (const Move move : ordered_moves(position, legal_moves, tt_move)) {
        StateInfo state;
        if (!position.make_move(move, state)) {
            continue;
        }
        context.keys.push_back(position.key());
        std::vector<Move> child_pv;
        const int score = -negamax(position, depth - 1, -beta, -alpha, ply + 1, context, child_pv);
        context.keys.pop_back();
        position.unmake_move(move, state);

        if (context.stopped) {
            return 0;
        }
        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
        if (score > alpha) {
            alpha = score;
            pv = {move};
            pv.insert(pv.end(), child_pv.begin(), child_pv.end());
        }
        if (alpha >= beta) {
            break;
        }
    }

    Bound bound = Bound::Exact;
    if (best_score <= original_alpha) {
        bound = Bound::Upper;
    } else if (best_score >= beta) {
        bound = Bound::Lower;
    }
    table_.store(position.key(), best_move, best_score, depth, bound, ply);
    return best_score;
}

int Searcher::quiescence(
    Position& position,
    int alpha,
    int beta,
    int ply,
    Context& context,
    std::vector<Move>& pv) {
    pv.clear();
    if (should_stop(context)) {
        return 0;
    }
    ++context.nodes;

    MoveList legal_moves;
    generate_legal(position, legal_moves);
    if (legal_moves.empty()) {
        return in_check(position) ? -search_mate_score + ply : 0;
    }
    if (position.rule50() >= 100 || is_repetition(context, position.key())) {
        return 0;
    }
    if (ply >= maximum_ply) {
        return evaluate(position);
    }

    const bool checked = in_check(position);
    if (!checked) {
        const int stand_pat = evaluate(position);
        if (stand_pat >= beta) {
            return stand_pat;
        }
        alpha = std::max(alpha, stand_pat);
    }

    for (const Move move : ordered_moves(position, legal_moves, Move{})) {
        if (!checked && !move.has_flag(MoveFlag::Capture) &&
            !move.has_flag(MoveFlag::EnPassant) && !move.has_flag(MoveFlag::Promotion)) {
            continue;
        }
        StateInfo state;
        if (!position.make_move(move, state)) {
            continue;
        }
        context.keys.push_back(position.key());
        std::vector<Move> child_pv;
        const int score = -quiescence(position, -beta, -alpha, ply + 1, context, child_pv);
        context.keys.pop_back();
        position.unmake_move(move, state);
        if (context.stopped) {
            return 0;
        }
        if (score > alpha) {
            alpha = score;
            pv = {move};
            pv.insert(pv.end(), child_pv.begin(), child_pv.end());
            if (alpha >= beta) {
                break;
            }
        }
    }
    return alpha;
}

bool Searcher::should_stop(Context& context) const {
    if (context.stopped) {
        return true;
    }
    if (context.external_stop != nullptr && context.external_stop->load(std::memory_order_relaxed)) {
        context.stopped = true;
    } else if (context.limits.nodes > 0 && context.nodes >= context.limits.nodes) {
        context.stopped = true;
    } else if (context.limits.move_time.count() > 0 &&
               std::chrono::steady_clock::now() - context.start >= context.limits.move_time) {
        context.stopped = true;
    }
    return context.stopped;
}

bool Searcher::is_repetition(const Context& context, std::uint64_t key) {
    return std::count(context.keys.begin(), context.keys.end(), key) >= 3;
}

}  // namespace blaze
