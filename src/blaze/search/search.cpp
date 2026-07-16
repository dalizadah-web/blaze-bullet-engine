#include "blaze/search/search.h"

#include "blaze/core/movegen.h"
#include "blaze/eval/classical.h"
#include "blaze/search/see.h"

#include <algorithm>
#include <array>
#include <bit>
#include <utility>
#include <vector>
#include <thread>

namespace blaze {
namespace {

constexpr int infinity = search_mate_score + 1;
constexpr int maximum_ply = 128;
const std::array<std::array<int, 64>, 64> empty_history{};

bool has_non_pawn_material(const Position& position, Color color) {
    return (position.pieces(color, PieceType::Knight) |
            position.pieces(color, PieceType::Bishop) |
            position.pieces(color, PieceType::Rook) |
            position.pieces(color, PieceType::Queen)) != 0;
}

bool king_is_safe_after_move(const Position& position, Color moving_side) {
    const Bitboard king = position.pieces(moving_side, PieceType::King);
    if (king == 0) {
        return false;
    }
    const Square king_square = static_cast<Square>(std::countr_zero(king));
    return !is_square_attacked(position, king_square, opposite(moving_side));
}

bool has_any_legal_move(Position& position, const MoveList& candidates) {
    const Color moving_side = position.side_to_move();
    for (const Move move : candidates) {
        StateInfo state;
        if (!position.make_move(move, state)) {
            continue;
        }
        const bool legal = king_is_safe_after_move(position, moving_side);
        position.unmake_move(move, state);
        if (legal) {
            return true;
        }
    }
    return false;
}

int victim_value(Piece piece) {
    constexpr std::array<int, 7> values = {0, 100, 320, 335, 500, 900, 20000};
    return values[static_cast<std::size_t>(type_of(piece))];
}

int move_order_score(
    const Position& position,
    Move move,
    Move tt_move,
    Move first_killer,
    Move second_killer,
    const std::array<std::array<int, 64>, 64>& history,
    Move counter_move) {
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
        score += std::clamp(static_exchange_evaluation(position, move), -4'000, 4'000) / 8;
    }
    if (move.has_flag(MoveFlag::Promotion)) {
        score += 80'000 + victim_value(make_piece(position.side_to_move(), move.promotion()));
    }
    const bool quiet = !move.has_flag(MoveFlag::Capture) &&
        !move.has_flag(MoveFlag::EnPassant) && !move.has_flag(MoveFlag::Promotion);
    if (quiet && move == first_killer) {
        score += 90'000;
    } else if (quiet && move == second_killer) {
        score += 89'000;
    } else if (quiet) {
        if (counter_move.is_valid() && move == counter_move) score += 85'000;
        score += history[static_cast<std::size_t>(square_index(move.from()))]
                        [static_cast<std::size_t>(square_index(move.to()))];
    }
    return score;
}

MoveList ordered_moves(
    const Position& position,
    const MoveList& list,
    Move tt_move,
    Move first_killer,
    Move second_killer,
    const std::array<std::array<int, 64>, 64>& history,
    Move counter_move = Move{}) {
    MoveList result = list;
    std::array<int, MoveList::capacity> scores;
    for (std::size_t index = 0; index < result.size(); ++index) {
        scores[index] = move_order_score(
            position, result[index], tt_move, first_killer, second_killer, history, counter_move);
    }
    for (std::size_t index = 1; index < result.size(); ++index) {
        const Move move = result[index];
        const int score = scores[index];
        std::size_t insertion = index;
        while (insertion > 0 && scores[insertion - 1] < score) {
            result[insertion] = result[insertion - 1];
            scores[insertion] = scores[insertion - 1];
            --insertion;
        }
        result[insertion] = move;
        scores[insertion] = score;
    }
    return result;
}

}  // namespace

SearchResult Searcher::search(
    Position position,
    const SearchLimits& limits,
    const std::atomic<bool>* external_stop,
    const std::vector<std::uint64_t>& prior_keys) {
    if (limits.threads > 1) {
        return search_parallel(position, limits, external_stop, prior_keys);
    }
    killers_ = {};
    countermoves_ = {};
    history_ = {};

    MoveList generated_moves;
    generate_legal(position, generated_moves);
    MoveList legal_moves;
    if (limits.search_moves.empty()) {
        legal_moves = generated_moves;
    } else {
        for (const Move requested : limits.search_moves) {
            for (const Move legal : generated_moves) {
                if (legal == requested ||
                    (legal.from() == requested.from() && legal.to() == requested.to() &&
                     legal.promotion() == requested.promotion())) {
                    legal_moves.push(legal);
                    break;
                }
            }
        }
    }

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
    context.root_moves.assign(legal_moves.begin(), legal_moves.end());

    if (should_stop(context)) {
        result.stopped = true;
        return result;
    }

    table_.new_search();
    const int maximum_depth = limits.depth > 0 ? limits.depth : 64;
    for (int depth = 1; depth <= maximum_depth; ++depth) {
        std::vector<Move> pv;
        int alpha = -infinity;
        int beta = infinity;
        if (depth >= 4 && result.depth >= 3) {
            alpha = std::max(-infinity, result.score - 50);
            beta = std::min(infinity, result.score + 50);
        }
        int score = negamax(position, depth, alpha, beta, 0, context, pv);
        if (!context.stopped && (score <= alpha || score >= beta)) {
            pv.clear();
            score = negamax(position, depth, -infinity, infinity, 0, context, pv);
        }
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

SearchResult Searcher::search_parallel(
    Position position,
    const SearchLimits& limits,
    const std::atomic<bool>* external_stop,
    const std::vector<std::uint64_t>& prior_keys) {
    MoveList generated_moves;
    generate_legal(position, generated_moves);
    MoveList legal_moves;
    if (limits.search_moves.empty()) {
        legal_moves = generated_moves;
    } else {
        for (const Move requested : limits.search_moves) {
            for (const Move legal : generated_moves) {
                if (legal.from() == requested.from() && legal.to() == requested.to() &&
                    legal.promotion() == requested.promotion()) {
                    legal_moves.push(legal);
                    break;
                }
            }
        }
    }

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
    if (external_stop != nullptr && external_stop->load(std::memory_order_relaxed)) {
        result.stopped = true;
        return result;
    }

    const int maximum_depth = limits.depth > 0 ? limits.depth : 64;
    const auto search_start = std::chrono::steady_clock::now();
    const auto shared_node_budget = limits.nodes > 0
        ? std::make_shared<std::atomic<std::uint64_t>>(limits.nodes)
        : std::shared_ptr<std::atomic<std::uint64_t>>{};
    std::atomic<bool> parallel_stop = false;
    for (int depth = 1; depth <= maximum_depth; ++depth) {
        if (external_stop != nullptr && external_stop->load(std::memory_order_relaxed)) {
            result.stopped = true;
            break;
        }
        if (depth == 1) {
            result.depth = 1;
            result.score = evaluate_position(position);
            continue;
        }

        struct TaskResult {
            Move move;
            int score = -infinity;
            std::uint64_t nodes = 0;
            std::vector<Move> pv;
            bool complete = false;
        };
        std::vector<TaskResult> tasks(legal_moves.size());
        for (std::size_t index = 0; index < legal_moves.size(); ++index) {
            tasks[index].move = legal_moves[index];
        }
        std::atomic<std::size_t> next_task = 0;
        std::atomic<int> shared_alpha{-infinity};
        table_.new_search();
        const unsigned worker_count = std::min<unsigned>(
            static_cast<unsigned>(limits.threads),
            static_cast<unsigned>(legal_moves.size()));
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (unsigned worker_index = 0; worker_index < worker_count; ++worker_index) {
            workers.emplace_back([&, depth] {
                while (!parallel_stop.load(std::memory_order_relaxed)) {
                    if (external_stop != nullptr && external_stop->load(std::memory_order_relaxed)) {
                        parallel_stop.store(true, std::memory_order_relaxed);
                        break;
                    }
                    const std::size_t index = next_task.fetch_add(1, std::memory_order_relaxed);
                    if (index >= tasks.size()) {
                        break;
                    }
                    Position child = position;
                    StateInfo state;
                    const Move root_move = tasks[index].move;
                    if (!child.make_move(root_move, state)) {
                        continue;
                    }
                    SearchLimits child_limits = limits;
                    child_limits.threads = 1;
                    child_limits.depth = depth - 1;
                    child_limits.search_moves.clear();
                    child_limits.nodes = 0;
                    child_limits.shared_node_budget = shared_node_budget;
                    std::vector<std::uint64_t> child_history = prior_keys;
                    child_history.push_back(position.key());
                    Searcher child_searcher(table_, network_);
                    const int observed_alpha = shared_alpha.load(std::memory_order_relaxed);
                    const int child_alpha = observed_alpha == -infinity
                        ? -infinity
                        : -observed_alpha - 1;
                    const int child_beta = observed_alpha == -infinity
                        ? infinity
                        : -observed_alpha;
                    SearchResult child_result = child_searcher.search_window(
                        child,
                        child_limits,
                        depth - 1,
                        child_alpha,
                        child_beta,
                        external_stop,
                        child_history,
                        search_start);
                    int score = -child_result.score;
                    if (!child_result.stopped && score > observed_alpha &&
                        observed_alpha != -infinity) {
                        child_result = child_searcher.search_window(
                            child,
                            child_limits,
                            depth - 1,
                            -infinity,
                            infinity,
                            external_stop,
                            child_history,
                            search_start);
                        score = -child_result.score;
                    }
                    TaskResult& task = tasks[index];
                    task.score = -child_result.score;
                    task.nodes = child_result.nodes;
                    task.pv.push_back(root_move);
                    task.pv.insert(task.pv.end(), child_result.pv.begin(), child_result.pv.end());
                    task.complete = !child_result.stopped;
                    if (task.complete) {
                        int previous = shared_alpha.load(std::memory_order_relaxed);
                        while (score > previous &&
                               !shared_alpha.compare_exchange_weak(
                                   previous, score, std::memory_order_relaxed)) {
                        }
                    } else {
                        parallel_stop.store(true, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        if (external_stop != nullptr && external_stop->load(std::memory_order_relaxed)) {
            result.stopped = true;
            break;
        }

        const TaskResult* best = nullptr;
        std::uint64_t searched_nodes = 0;
        for (const TaskResult& task : tasks) {
            searched_nodes += task.nodes;
            if (task.complete && (best == nullptr || task.score > best->score)) {
                best = &task;
            }
        }
        result.nodes += searched_nodes;
        if (best == nullptr) {
            result.stopped = true;
            break;
        }
        result.best_move = best->move;
        result.score = best->score;
        result.pv = best->pv;
        result.depth = depth;
        if (result.score >= search_mate_threshold || result.score <= -search_mate_threshold) {
            break;
        }
    }
    return result;
}

SearchResult Searcher::search_window(
    Position position,
    const SearchLimits& limits,
    int depth,
    int alpha,
    int beta,
    const std::atomic<bool>* external_stop,
    const std::vector<std::uint64_t>& prior_keys,
    std::chrono::steady_clock::time_point start) {
    killers_ = {};
    countermoves_ = {};
    history_ = {};

    Context context;
    context.limits = limits;
    context.external_stop = external_stop;
    context.start = start;
    context.keys = prior_keys;
    context.keys.push_back(position.key());

    SearchResult result;
    std::vector<Move> pv;
    const int score = negamax(position, depth, alpha, beta, 0, context, pv);
    if (!context.stopped) {
        result.score = score;
        result.depth = depth;
        result.pv = std::move(pv);
        if (!result.pv.empty()) result.best_move = result.pv.front();
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
    std::vector<Move>& pv,
    bool allow_null) {
    pv.clear();
    if (depth <= 0) {
        return quiescence(position, alpha, beta, ply, context, pv);
    }
    if (should_stop(context)) {
        return 0;
    }
    if (!consume_node(context)) {
        return 0;
    }

    MoveList legal_moves;
    generate_pseudo_legal(position, legal_moves);
    if (ply == 0 && !context.root_moves.empty()) {
        MoveList restricted;
        for (const Move candidate : legal_moves) {
            for (const Move requested : context.root_moves) {
                if (candidate.from() == requested.from() &&
                    candidate.to() == requested.to() &&
                    candidate.promotion() == requested.promotion()) {
                    restricted.push(candidate);
                    break;
                }
            }
        }
        legal_moves = restricted;
    }
    if (ply >= maximum_ply) {
        return evaluate_position(position);
    }
    const bool checked = in_check(position);
    if (position.rule50() >= 100 || is_repetition(context, position.key())) {
        if (!checked) {
            return 0;
        }
        MoveList evasions;
        generate_legal(position, evasions);
        return evasions.empty() ? -search_mate_score + ply : 0;
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

    if (allow_null && depth >= 3 && !checked && position.rule50() < 99 &&
        beta < search_mate_threshold &&
        has_non_pawn_material(position, position.side_to_move())) {
        StateInfo null_state;
        position.make_null(null_state);
        std::vector<Move> null_pv;
        const int reduction = depth >= 6 ? 3 : 2;
        const Move saved_previous_move = context.previous_move;
        context.previous_move = Move{};
        const int null_score = -negamax(
            position,
            depth - 1 - reduction,
            -beta,
            -beta + 1,
            ply + 1,
            context,
            null_pv,
            false);
        context.previous_move = saved_previous_move;
        position.unmake_null(null_state);
        if (context.stopped) {
            return 0;
        }
        if (null_score >= beta) {
            return null_score;
        }
    }

    if (depth >= 5 && !checked && beta < search_mate_threshold - 180) {
        constexpr int probcut_margin = 180;
        MoveList tactical_moves;
        generate_pseudo_legal(position, tactical_moves);
        for (const Move move : ordered_moves(
                 position, tactical_moves, tt_move, Move{}, Move{}, empty_history)) {
            if (!move.has_flag(MoveFlag::Capture) &&
                !move.has_flag(MoveFlag::EnPassant) &&
                !move.has_flag(MoveFlag::Promotion)) {
                continue;
            }
            if (static_exchange_evaluation(position, move) < 0) continue;
            StateInfo state;
            if (!position.make_move(move, state)) continue;
            const Color moving_side = position.side_to_move();
            if (!king_is_safe_after_move(position, moving_side)) {
                position.unmake_move(move, state);
                continue;
            }
            context.keys.push_back(position.key());
            const Move saved_previous_move = context.previous_move;
            context.previous_move = move;
            std::vector<Move> probe_pv;
            const int probe_score = -negamax(
                position,
                depth - 4,
                -beta - probcut_margin,
                -beta,
                ply + 1,
                context,
                probe_pv);
            context.previous_move = saved_previous_move;
            context.keys.pop_back();
            position.unmake_move(move, state);
            if (context.stopped) return 0;
            if (probe_score >= beta + probcut_margin) return probe_score;
        }
    }

    const int original_alpha = alpha;
    Move best_move;
    int best_score = -infinity;
    int legal_count = 0;
    const std::size_t color_index = static_cast<std::size_t>(position.side_to_move());
    const Move first_killer = killers_[static_cast<std::size_t>(ply)][0];
    const Move second_killer = killers_[static_cast<std::size_t>(ply)][1];
    int move_count = 0;
    for (const Move move : ordered_moves(
             position,
             legal_moves,
             tt_move,
             first_killer,
             second_killer,
             history_[color_index],
             context.previous_move.is_valid()
                 ? countermoves_[square_index(context.previous_move.from())]
                     [square_index(context.previous_move.to())]
                 : Move{})) {
        StateInfo state;
        if (!position.make_move(move, state)) {
            continue;
        }
        if (!king_is_safe_after_move(position, opposite(position.side_to_move()))) {
            position.unmake_move(move, state);
            continue;
        }
        ++legal_count;
        context.keys.push_back(position.key());
        std::vector<Move> child_pv;
        const bool gives_check = in_check(position);
        const bool recaptures = context.previous_move.is_valid() &&
            context.previous_move.to() == move.to() &&
            (context.previous_move.has_flag(MoveFlag::Capture) ||
             context.previous_move.has_flag(MoveFlag::EnPassant)) &&
            (move.has_flag(MoveFlag::Capture) || move.has_flag(MoveFlag::EnPassant));
        const int full_depth = depth - 1 + (gives_check ? 1 : 0) + (recaptures ? 1 : 0);
        int score = 0;
        const Move saved_previous_move = context.previous_move;
        context.previous_move = move;
        if (move_count == 0) {
            score = -negamax(position, full_depth, -beta, -alpha, ply + 1, context, child_pv);
        } else {
            const bool quiet = !move.has_flag(MoveFlag::Capture) &&
                !move.has_flag(MoveFlag::EnPassant) && !move.has_flag(MoveFlag::Promotion);
            int reduction = 0;
            if (depth >= 3 && move_count >= 3 && quiet && !checked && !gives_check) {
                reduction = 1;
                if (depth >= 5 && move_count >= 6) {
                    ++reduction;
                }
                if (depth >= 8 && move_count >= 12) ++reduction;
            }
            score = -negamax(
                position,
                full_depth - reduction,
                -alpha - 1,
                -alpha,
                ply + 1,
                context,
                child_pv);
            if (!context.stopped && reduction > 0 && score > alpha) {
                score = -negamax(
                    position,
                    full_depth,
                    -alpha - 1,
                    -alpha,
                    ply + 1,
                    context,
                    child_pv);
            }
            if (!context.stopped && score > alpha && score < beta) {
                score = -negamax(position, full_depth, -beta, -alpha, ply + 1, context, child_pv);
            }
        }
        context.previous_move = saved_previous_move;
        ++move_count;
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
            const bool quiet = !move.has_flag(MoveFlag::Capture) &&
                !move.has_flag(MoveFlag::EnPassant) && !move.has_flag(MoveFlag::Promotion);
            if (quiet) {
                auto& killers = killers_[static_cast<std::size_t>(ply)];
                if (move != killers[0]) {
                    killers[1] = killers[0];
                    killers[0] = move;
                }
                int& history = history_[color_index]
                    [static_cast<std::size_t>(square_index(move.from()))]
                    [static_cast<std::size_t>(square_index(move.to()))];
                history = std::min(history + depth * depth * 16, 80'000);
                if (context.previous_move.is_valid()) {
                    countermoves_[square_index(context.previous_move.from())]
                        [square_index(context.previous_move.to())] = move;
                }
            }
            break;
        }
    }

    if (legal_count == 0) {
        return checked ? -search_mate_score + ply : 0;
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
    if (!consume_node(context)) {
        return 0;
    }

    MoveList legal_moves;
    generate_pseudo_legal(position, legal_moves);
    if (ply >= maximum_ply) {
        return evaluate_position(position);
    }

    const bool checked = in_check(position);
    if (!checked && !has_any_legal_move(position, legal_moves)) {
        return 0;
    }
    if (position.rule50() >= 100 || is_repetition(context, position.key())) {
        if (!checked) {
            return 0;
        }
        return has_any_legal_move(position, legal_moves)
            ? 0
            : -search_mate_score + ply;
    }
    int stand_pat = -infinity;
    if (!checked) {
        stand_pat = evaluate_position(position);
        if (stand_pat >= beta) {
            return stand_pat;
        }
        alpha = std::max(alpha, stand_pat);
    }

    int legal_count = 0;
    for (const Move move : ordered_moves(
             position, legal_moves, Move{}, Move{}, Move{}, empty_history)) {
        if (!checked && !move.has_flag(MoveFlag::Capture) &&
            !move.has_flag(MoveFlag::EnPassant) && !move.has_flag(MoveFlag::Promotion)) {
            continue;
        }
        StateInfo state;
        if (!position.make_move(move, state)) {
            continue;
        }
        if (!king_is_safe_after_move(position, opposite(position.side_to_move()))) {
            position.unmake_move(move, state);
            continue;
        }
        ++legal_count;
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
    if (checked && legal_count == 0) {
        return -search_mate_score + ply;
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

bool Searcher::consume_node(Context& context) const {
    if (should_stop(context)) {
        return false;
    }
    if (context.limits.shared_node_budget) {
        std::uint64_t remaining =
            context.limits.shared_node_budget->load(std::memory_order_relaxed);
        while (remaining != 0 &&
               !context.limits.shared_node_budget->compare_exchange_weak(
                   remaining,
                   remaining - 1,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        if (remaining == 0) {
            context.stopped = true;
            return false;
        }
    }
    ++context.nodes;
    return true;
}

int Searcher::evaluate_position(const Position& position) const {
    constexpr std::size_t cache_mask = 4095U;
    const std::uint64_t key = position.key();
    EvalCacheEntry& entry = eval_cache_[static_cast<std::size_t>(key) & cache_mask];
    if (entry.valid && entry.key == key) return entry.score;
    const int score = network_ != nullptr ? network_->evaluate(position) : evaluate(position);
    entry = EvalCacheEntry{key, score, true};
    return score;
}

bool Searcher::is_repetition(const Context& context, std::uint64_t key) {
    return std::count(context.keys.begin(), context.keys.end(), key) >= 3;
}

}  // namespace blaze
