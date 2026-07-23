#include "blaze/search/search.h"

#include "blaze/core/movegen.h"
#include "blaze/eval/classical.h"
#include "blaze/search/see.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace blaze {
namespace {

constexpr int infinity = search_mate_score + 1;
constexpr int maximum_ply = 128;
constexpr int maximum_extensions = 2;

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

}  // namespace

SearchResult Searcher::search(
    Position position,
    const SearchLimits& limits,
    const std::atomic<bool>* external_stop,
    const std::vector<std::uint64_t>& prior_keys) {
    if (limits.threads > 1) {
        return search_parallel(position, limits, external_stop, prior_keys);
    }
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
    context.stack[0].extension_count = 0;
#ifndef NDEBUG
    context.limits.maximum_ply = std::clamp(
        context.limits.maximum_ply,
        1,
        maximum_ply);
#endif
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
        PvLine pv;
        int alpha = -infinity;
        int beta = infinity;
        if (depth >= 4 && result.depth >= 3) {
            alpha = std::max(-infinity, result.score - 50);
            beta = std::min(infinity, result.score + 50);
        }
        int score = negamax<NodeType::Root>(position, depth, alpha, beta, 0, context, pv);
        if (!context.stopped && (score <= alpha || score >= beta)) {
            pv.clear();
            score = negamax<NodeType::Root>(
                position, depth, -infinity, infinity, 0, context, pv);
        }
        if (context.stopped) {
            break;
        }
        if (!pv.empty()) {
            result.best_move = pv.front();
            result.pv.assign(pv.span().begin(), pv.span().end());
        }
        result.score = score;
        result.depth = depth;
        if (score >= search_mate_threshold || score <= -search_mate_threshold) {
            break;
        }
        if (limits.target_time.count() > 0 &&
            std::chrono::steady_clock::now() - context.start >= limits.target_time) {
            break;
        }
    }

    result.nodes = context.nodes;
#ifndef NDEBUG
    result.maximum_extension_count = context.maximum_extension_count;
    result.effective_maximum_ply = context.limits.maximum_ply;
    result.probcut_legal_checks = context.probcut_legal_checks;
    result.null_move_searches = context.null_move_searches;
    result.null_move_pv_searches = context.null_move_pv_searches;
    result.null_move_verifications = context.null_move_verifications;
#endif
    result.stopped = context.stopped;
    result.picker_stats = context.picker_stats;
    return result;
}

#ifndef NDEBUG
SearchResult Searcher::debug_search_window(
    Position position,
    int depth,
    int alpha,
    int beta) {
    SearchLimits limits{.depth = depth};
    return search_window(
        std::move(position),
        limits,
        depth,
        0,
        Move{},
        0,
        alpha,
        beta,
        nullptr,
        {},
        std::chrono::steady_clock::now());
}
#endif

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
#ifndef NDEBUG
    result.effective_maximum_ply = std::clamp(limits.maximum_ply, 1, maximum_ply);
#endif
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
    table_.new_search();
    for (int depth = 1; depth <= maximum_depth; ++depth) {
        if (external_stop != nullptr && external_stop->load(std::memory_order_relaxed)) {
            result.stopped = true;
            break;
        }
        struct TaskResult {
            Move move;
            int score = -infinity;
            std::uint64_t nodes = 0;
#ifndef NDEBUG
            int maximum_extension_count = 0;
            std::uint64_t null_move_searches = 0;
            std::uint64_t null_move_pv_searches = 0;
            std::uint64_t null_move_verifications = 0;
#endif
            std::vector<Move> pv;
            bool complete = false;
        };
        std::vector<TaskResult> tasks(legal_moves.size());
        for (std::size_t index = 0; index < legal_moves.size(); ++index) {
            tasks[index].move = legal_moves[index];
        }
        // Set root aspiration window based on the previous iteration's score
        int root_alpha = -infinity;
        int root_beta = infinity;
        if (depth >= 4 && result.depth >= 3 && result.score > -search_mate_score + 1) {
            root_alpha = std::max(-infinity, result.score - 50);
            root_beta = std::min(infinity, result.score + 50);
        }
        std::atomic<int> shared_alpha{root_alpha};
        std::atomic<std::size_t> next_task{0};
        // Search first root move serially to establish alpha before parallel workers
        {
            Position child = position;
            StateInfo state;
            const Move root_move = tasks[0].move;
            if (child.make_move(root_move, state)) {
                const int root_extension = in_check(child) && depth >= 3 ? 1 : 0;
                const int child_depth = depth - 1 + root_extension;
                SearchLimits child_limits = limits;
                child_limits.threads = 1;
                child_limits.depth = child_depth;
                child_limits.search_moves.clear();
                child_limits.nodes = 0;
                child_limits.shared_node_budget = shared_node_budget;
                std::vector<std::uint64_t> child_history = prior_keys;
                child_history.push_back(position.key());
                Searcher child_searcher(table_, network_);
                SearchResult child_result = child_searcher.search_window(
                    child, child_limits, child_depth, 1, root_move, root_extension,
                    root_alpha, root_beta, external_stop, child_history, search_start);
                int score = -child_result.score;
                // Re-search with full window if the result falls outside the
                // aspiration window, or if no aspiration was set (alpha=-inf).
                if (!child_result.stopped && (score <= root_alpha || score >= root_beta)) {
                    child_result = child_searcher.search_window(
                        child, child_limits, child_depth, 1, root_move, root_extension,
                        -infinity, infinity, external_stop, child_history, search_start);
                    score = -child_result.score;
                }
                TaskResult& task = tasks[0];
                task.score = score;
                task.nodes = child_result.nodes;
#ifndef NDEBUG
                task.maximum_extension_count = child_result.maximum_extension_count;
                task.null_move_searches = child_result.null_move_searches;
                task.null_move_pv_searches = child_result.null_move_pv_searches;
                task.null_move_verifications = child_result.null_move_verifications;
#endif
                task.pv.push_back(root_move);
                task.pv.insert(task.pv.end(), child_result.pv.begin(), child_result.pv.end());
                task.complete = !child_result.stopped;
                if (task.complete) {
                    if (score > shared_alpha.load(std::memory_order_relaxed)) {
                        shared_alpha.store(score, std::memory_order_relaxed);
                    }
                } else {
                    parallel_stop.store(true, std::memory_order_relaxed);
                }
            }
        }
        next_task.store(1, std::memory_order_relaxed);
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
                    const int root_extension =
                        in_check(child) && depth >= 3 && index < 4 ? 1 : 0;
                    const int child_depth = depth - 1 + root_extension;
                    SearchLimits child_limits = limits;
                    child_limits.threads = 1;
                    child_limits.depth = child_depth;
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
                        child_depth,
                        1,
                        root_move,
                        root_extension,
                        child_alpha,
                        child_beta,
                        external_stop,
                        child_history,
                        search_start);
#ifndef NDEBUG
                    int maximum_extension_count = child_result.maximum_extension_count;
                    std::uint64_t null_move_searches = child_result.null_move_searches;
                    std::uint64_t null_move_pv_searches = child_result.null_move_pv_searches;
                    std::uint64_t null_move_verifications =
                        child_result.null_move_verifications;
#endif
                    int score = -child_result.score;
                    if (!child_result.stopped && score > observed_alpha &&
                        observed_alpha != -infinity) {
                        child_result = child_searcher.search_window(
                            child,
                            child_limits,
                            child_depth,
                            1,
                            root_move,
                            root_extension,
                            -infinity,
                            infinity,
                            external_stop,
                            child_history,
                            search_start);
#ifndef NDEBUG
                        maximum_extension_count = std::max(
                            maximum_extension_count,
                            child_result.maximum_extension_count);
                        null_move_searches += child_result.null_move_searches;
                        null_move_pv_searches += child_result.null_move_pv_searches;
                        null_move_verifications += child_result.null_move_verifications;
#endif
                        score = -child_result.score;
                    }
                    TaskResult& task = tasks[index];
                    task.score = -child_result.score;
                    task.nodes = child_result.nodes;
#ifndef NDEBUG
                    task.maximum_extension_count = maximum_extension_count;
                    task.null_move_searches = null_move_searches;
                    task.null_move_pv_searches = null_move_pv_searches;
                    task.null_move_verifications = null_move_verifications;
#endif
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
#ifndef NDEBUG
            result.maximum_extension_count = std::max(
                result.maximum_extension_count,
                task.maximum_extension_count);
            result.null_move_searches += task.null_move_searches;
            result.null_move_pv_searches += task.null_move_pv_searches;
            result.null_move_verifications += task.null_move_verifications;
#endif
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
        if (limits.target_time.count() > 0 &&
            std::chrono::steady_clock::now() - search_start >= limits.target_time) {
            break;
        }
    }
    return result;
}

SearchResult Searcher::search_window(
    Position position,
    const SearchLimits& limits,
    int depth,
    int ply,
    Move previous_move,
    int extension_count,
    int alpha,
    int beta,
    const std::atomic<bool>* external_stop,
    const std::vector<std::uint64_t>& prior_keys,
    std::chrono::steady_clock::time_point start) {
    Context context;
    context.limits = limits;
    context.stack[0].extension_count = 0;
#ifndef NDEBUG
    context.limits.maximum_ply = std::clamp(
        context.limits.maximum_ply,
        1,
        maximum_ply);
    context.maximum_extension_count = extension_count;
#endif
    context.stack[static_cast<std::size_t>(ply)].current_move = previous_move;
    context.stack[static_cast<std::size_t>(ply)].extension_count = extension_count;
    context.external_stop = external_stop;
    context.start = start;
    context.keys = prior_keys;
    context.keys.push_back(position.key());

    SearchResult result;
    PvLine pv;
    const int score = negamax<NodeType::Root>(position, depth, alpha, beta, ply, context, pv);
    if (!context.stopped) {
        result.score = score;
        result.depth = depth;
        result.pv.assign(pv.span().begin(), pv.span().end());
        if (!result.pv.empty()) result.best_move = result.pv.front();
    }
    result.nodes = context.nodes;
#ifndef NDEBUG
    result.maximum_extension_count = context.maximum_extension_count;
    result.effective_maximum_ply = context.limits.maximum_ply;
    result.null_move_searches = context.null_move_searches;
    result.null_move_pv_searches = context.null_move_pv_searches;
    result.null_move_verifications = context.null_move_verifications;
#endif
    result.stopped = context.stopped;
    result.picker_stats = context.picker_stats;
    return result;
}

template<Searcher::NodeType node_type>
int Searcher::negamax(
    Position& position,
    int depth,
    int alpha,
    int beta,
    int ply,
    Context& context,
    PvLine& pv,
    bool allow_null) {
    constexpr bool pv_node = node_type != NodeType::NonPV;
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
    if constexpr (node_type == NodeType::Root) {
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
    }
#ifndef NDEBUG
    if (ply >= context.limits.maximum_ply) {
#else
    if (ply >= maximum_ply) {
#endif
        return maximum_ply_score(position, ply);
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

    alpha = std::max(alpha, -search_mate_score + ply);
    beta = std::min(beta, search_mate_score - ply - 1);
    if (alpha >= beta) {
        return alpha;
    }

    Move tt_move;
    int static_eval = tt_no_static_evaluation;
    const auto tt_hit = table_.probe(position.key(), ply, position.rule50());
    if (tt_hit) {
        tt_move = tt_hit->move;
        static_eval = tt_hit->static_evaluation;
        if (static_eval != tt_no_static_evaluation) {
            context.stack[static_cast<std::size_t>(ply)].static_evaluation = static_eval;
        }
        const bool rule50_safe = tt_hit->rule50 == std::min(position.rule50(), 100);
        if (rule50_safe && tt_hit->depth >= depth) {
            if (tt_hit->bound == Bound::Exact ||
                (tt_hit->bound == Bound::Lower && tt_hit->score >= beta) ||
                (tt_hit->bound == Bound::Upper && tt_hit->score <= alpha)) {
                return tt_hit->score;
            }
        }
    }

    if constexpr (node_type == NodeType::NonPV) {
        // Keep deep verification nodes tied to a fresh leaf estimate. This
        // avoids reusing a stale bound's optional metadata while preserving
        // the hot-path benefit at shallow/selective nodes.
        if (static_eval == tt_no_static_evaluation || depth >= 10) {
            static_eval = evaluate_position(position);
        }
        context.stack[static_cast<std::size_t>(ply)].static_evaluation = static_eval;
        const bool null_enabled =
#ifndef NDEBUG
            context.limits.enable_null_move;
#else
            true;
#endif
        if (null_enabled && allow_null && depth >= 3 && !checked &&
            position.rule50() < 90 && beta < search_mate_threshold &&
            static_eval >= beta &&
            has_non_pawn_material(position, position.side_to_move())) {
#ifndef NDEBUG
            const auto record_null_move_attempt = [&context] {
                ++context.null_move_searches;
                if constexpr (node_type != NodeType::NonPV) {
                    ++context.null_move_pv_searches;
                }
            };
            record_null_move_attempt();
#endif
            const int eval_term = std::clamp((static_eval - beta) / 180, 0, 3);
            const int reduction = std::min(depth - 1, 3 + depth / 4 + eval_term);
            StateInfo null_state;
            position.make_null(null_state);
            table_.prefetch(position.key());
            PvLine null_pv;
            context.stack[static_cast<std::size_t>(ply + 1)].current_move = Move{};
            context.stack[static_cast<std::size_t>(ply + 1)].extension_count =
                context.stack[static_cast<std::size_t>(ply)].extension_count;
            const int null_score = -negamax<NodeType::NonPV>(
                position,
                depth - 1 - reduction,
                -beta,
                -beta + 1,
                ply + 1,
                context,
                null_pv,
                false);
            position.unmake_null(null_state);
            if (context.stopped) {
                return 0;
            }
            if (null_score >= beta) {
                if (depth < 10) {
                    return std::min(null_score, search_mate_threshold - 1);
                }
#ifndef NDEBUG
                ++context.null_move_verifications;
#endif
                PvLine verification_pv;
                const int verification = negamax<NodeType::NonPV>(
                    position,
                    depth - reduction,
                    beta - 1,
                    beta,
                    ply,
                    context,
                    verification_pv,
                    false);
                if (context.stopped) {
                    return 0;
                }
                if (verification >= beta) {
                    return verification;
                }
            }
        }
    }

    bool probcut_enabled = true;
#ifndef NDEBUG
    probcut_enabled = context.limits.enable_probcut;
#endif
    if (probcut_enabled && depth >= 5 && !checked && beta < search_mate_threshold - 180) {
        constexpr int probcut_margin = 180;
        MoveList tactical_moves;
        generate_pseudo_legal(position, tactical_moves);
        for (std::size_t i = 0; i < tactical_moves.size(); ++i) {
            const Move move = tactical_moves[i];
            if (!move.has_flag(MoveFlag::Capture) &&
                !move.has_flag(MoveFlag::EnPassant) &&
                !move.has_flag(MoveFlag::Promotion)) {
                continue;
            }
            if (static_exchange_evaluation(position, move) < 0) continue;
            StateInfo state;
            const Color moving_side = position.side_to_move();
            if (!position.make_move(move, state)) continue;
            if (!king_is_safe_after_move(position, moving_side)) {
                position.unmake_move(move, state);
                continue;
            }
#ifndef NDEBUG
            if (in_check(position)) {
                ++context.probcut_legal_checks;
            }
#endif
            context.keys.push_back(position.key());
            table_.prefetch(position.key());
            context.stack[static_cast<std::size_t>(ply + 1)].current_move = move;
            context.stack[static_cast<std::size_t>(ply + 1)].extension_count =
                context.stack[static_cast<std::size_t>(ply)].extension_count;
            PvLine probe_pv;
            const int probe_score = -negamax<NodeType::NonPV>(
                position,
                depth - 4,
                -beta - probcut_margin,
                -beta,
                ply + 1,
                context,
                probe_pv);
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
    const Move previous_move = ply > 0
        ? context.stack[static_cast<std::size_t>(ply)].current_move
        : Move{};
    const Move counter_move = previous_move.is_valid()
        ? countermoves_[square_index(previous_move.from())][square_index(previous_move.to())]
        : Move{};

    MovePicker picker(position, legal_moves, tt_move,
                      context.stack[static_cast<std::size_t>(ply)].killers.data(),
                      counter_move,
                      position.side_to_move(),
                      history_[static_cast<std::size_t>(position.side_to_move())],
                      &capture_history_);

    int move_count = 0;
    while (true) {
        const Move move = picker.next();
        if (!move.is_valid()) break;
        ++move_count;
        const bool recaptures = previous_move.is_valid() &&
            previous_move.to() == move.to() &&
            (previous_move.has_flag(MoveFlag::Capture) ||
             previous_move.has_flag(MoveFlag::EnPassant)) &&
            (move.has_flag(MoveFlag::Capture) || move.has_flag(MoveFlag::EnPassant));
        const int see_score = recaptures
            ? static_exchange_evaluation(position, move)
            : std::numeric_limits<int>::min();
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
        table_.prefetch(position.key());
        PvLine child_pv;
        const bool gives_check = in_check(position);
        const int used = context.stack[static_cast<std::size_t>(ply)].extension_count;
        int extension = 0;
        if (used < maximum_extensions) {
            const bool selective_check = gives_check && depth >= 3 && move_count < 4;
            const bool sound_recapture = recaptures && depth <= 8 && see_score >= 0;
            extension = selective_check || sound_recapture ? 1 : 0;
        }
        const int child_extension_count = used + extension;
        context.stack[static_cast<std::size_t>(ply + 1)].extension_count =
            child_extension_count;
#ifndef NDEBUG
        context.maximum_extension_count = std::max(
            context.maximum_extension_count,
            child_extension_count);
#endif
        const int full_depth = depth - 1 + extension;
        int score = 0;
        context.stack[static_cast<std::size_t>(ply + 1)].current_move = move;
        if (move_count == 1) {
            if constexpr (pv_node) {
                score = -negamax<NodeType::PV>(
                    position, full_depth, -beta, -alpha, ply + 1, context, child_pv);
            } else {
                score = -negamax<NodeType::NonPV>(
                    position, full_depth, -beta, -alpha, ply + 1, context, child_pv);
            }
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
            score = -negamax<NodeType::NonPV>(
                position,
                full_depth - reduction,
                -alpha - 1,
                -alpha,
                ply + 1,
                context,
                child_pv);
            if (!context.stopped && reduction > 0 && score > alpha) {
                score = -negamax<NodeType::NonPV>(
                    position,
                    full_depth,
                    -alpha - 1,
                    -alpha,
                    ply + 1,
                    context,
                    child_pv);
            }
            if constexpr (pv_node) {
                if (!context.stopped && score > alpha && score < beta) {
                    score = -negamax<NodeType::PV>(
                        position, full_depth, -beta, -alpha, ply + 1, context, child_pv);
                }
            }
        }
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
            pv.prepend(move, child_pv);
        }
        if (alpha >= beta) {
            picker.on_cutoff(move_count);
            const bool quiet = !move.has_flag(MoveFlag::Capture) &&
                !move.has_flag(MoveFlag::EnPassant) && !move.has_flag(MoveFlag::Promotion);
            if (quiet) {
                auto& killers = context.stack[static_cast<std::size_t>(ply)].killers;
                if (move != killers[0]) {
                    killers[1] = killers[0];
                    killers[0] = move;
                }
                int& history = history_[color_index]
                    [static_cast<std::size_t>(square_index(move.from()))]
                    [static_cast<std::size_t>(square_index(move.to()))];
                history = std::min(history + depth * depth * 16, 80'000);
                if (previous_move.is_valid()) {
                    countermoves_[square_index(previous_move.from())]
                        [square_index(previous_move.to())] = move;
                }
            } else if (move.has_flag(MoveFlag::Capture) || move.has_flag(MoveFlag::EnPassant)) {
                const Piece victim = move.has_flag(MoveFlag::EnPassant)
                    ? make_piece(opposite(position.side_to_move()), PieceType::Pawn)
                    : position.piece_on(move.to());
                const Piece attacker = position.piece_on(move.from());
                int& ch = capture_history_
                    [static_cast<std::size_t>(type_of(victim))]
                    [static_cast<std::size_t>(type_of(attacker))]
                    [static_cast<std::size_t>(square_index(move.to()))];
                ch = std::clamp(ch + depth * depth, -16'384, 16'384);
            }
            break;
        }
    }

    context.picker_stats.accumulate(picker.collect_stats());

    if (legal_count == 0) {
        return checked ? -search_mate_score + ply : 0;
    }

    Bound bound = Bound::Exact;
    if (best_score <= original_alpha) {
        bound = Bound::Upper;
    } else if (best_score >= beta) {
        bound = Bound::Lower;
    }
    table_.store(
        position.key(),
        best_move,
        best_score,
        depth,
        bound,
        ply,
        position.rule50(),
        static_eval,
        pv_node);
    return best_score;
}

int Searcher::quiescence(
    Position& position,
    int alpha,
    int beta,
    int ply,
    Context& context,
    PvLine& pv) {
    pv.clear();
    if (should_stop(context)) {
        return 0;
    }
    if (!consume_node(context)) {
        return 0;
    }
    ++context.picker_stats.qnodes;

#ifndef NDEBUG
    if (ply >= context.limits.maximum_ply) {
#else
    if (ply >= maximum_ply) {
#endif
        return maximum_ply_score(position, ply);
    }

    const bool checked = in_check(position);

    // Clamp alpha/beta to mate-distance bounds (same as main search).
    // This ensures TT mate scores from different plies are compared safely.
    alpha = std::max(alpha, -search_mate_score + ply);
    beta = std::min(beta, search_mate_score - ply - 1);
    if (alpha >= beta) {
        return alpha;
    }

    // Transposition-table probe
    Move tt_move;
    int static_eval = tt_no_static_evaluation;
    if (!checked) {
        const auto tt_hit = table_.probe(position.key(), ply, position.rule50());
        if (tt_hit) {
            tt_move = tt_hit->move;
            static_eval = tt_hit->static_evaluation;
            if (static_eval != tt_no_static_evaluation) {
                context.stack[static_cast<std::size_t>(ply)].static_evaluation = static_eval;
            }
            const bool rule50_safe = tt_hit->rule50 == std::min(position.rule50(), 100);
            if (rule50_safe && tt_hit->depth >= 0) {
                if (tt_hit->bound == Bound::Exact) {
                    return tt_hit->score;
                }
                if (tt_hit->bound == Bound::Lower && tt_hit->score >= beta) {
                    return tt_hit->score;
                }
                if (tt_hit->bound == Bound::Upper && tt_hit->score <= alpha) {
                    return tt_hit->score;
                }
            }
        }
    } else {
        // In check: probe TT for the TT move only (no cutoffs, no static eval)
        const auto tt_hit = table_.probe(position.key(), ply, position.rule50());
        if (tt_hit) {
            tt_move = tt_hit->move;
        }
    }

    MoveList legal_moves;
    generate_pseudo_legal(
        position,
        legal_moves,
        checked ? GenType::All : GenType::Captures);
    if (!checked && !has_any_legal_move(position, legal_moves)) {
        MoveList quiet_moves;
        generate_pseudo_legal(position, quiet_moves, GenType::Quiets);
        if (!has_any_legal_move(position, quiet_moves)) return 0;
    }
    if (position.rule50() >= 100 || is_repetition(context, position.key())) {
        if (!checked) {
            return 0;
        }
        return has_any_legal_move(position, legal_moves)
            ? 0
            : -search_mate_score + ply;
    }

    const int original_alpha = alpha;
    int best_score = -infinity;
    Move best_move;

    // Stand-pat evaluation (use TT static_eval if available)
    if (!checked) {
        int stand_pat;
        if (static_eval != tt_no_static_evaluation) {
            stand_pat = static_eval;
        } else {
            stand_pat = evaluate_position(position);
            static_eval = stand_pat;
        }
        if (stand_pat >= beta) {
            table_.store(position.key(), Move{}, stand_pat, 0, Bound::Lower, ply,
                         position.rule50(), static_eval, false);
            return stand_pat;
        }
        alpha = std::max(alpha, stand_pat);
        best_score = stand_pat;
    }

    // Build tactical move buffer (fixed capacity), with TT move first
    std::array<std::pair<int, Move>, MoveList::capacity> q_buffer;
    int q_count = 0;

    bool tt_used = false;
    if (tt_move.is_valid()) {
        StateInfo tt_state;
        if (position.make_move(tt_move, tt_state)) {
            if (king_is_safe_after_move(position, opposite(position.side_to_move()))) {
                position.unmake_move(tt_move, tt_state);
                tt_used = true;
                q_buffer[static_cast<std::size_t>(q_count++)] = {2'000'000, tt_move};
            } else {
                position.unmake_move(tt_move, tt_state);
            }
        }
    }

    bool pruned_this_node = false;
    for (std::size_t i = 0; i < legal_moves.size(); ++i) {
        const Move m = legal_moves[i];
        if (tt_used && m == tt_move) continue;
        if (!checked && !m.has_flag(MoveFlag::Capture) &&
            !m.has_flag(MoveFlag::EnPassant) && !m.has_flag(MoveFlag::Promotion)) {
            continue;
        }

        // SEE pruning: only in non-check nodes, for ordinary captures only.
        // Never prune the TT move, promotions, en-passant, or checking captures.
        if (!checked && m.has_flag(MoveFlag::Capture) &&
            !m.has_flag(MoveFlag::Promotion) && !m.has_flag(MoveFlag::EnPassant)) {
            ++context.picker_stats.see_pruning_calls;
            if (!see_ge(position, m, 0)) {
                StateInfo si;
                bool gives_check = false;
                if (position.make_move(m, si)) {
                    gives_check = in_check(position);
                    position.unmake_move(m, si);
                }
                if (!gives_check) {
                    ++context.picker_stats.captures_pruned_by_see;
                    pruned_this_node = true;
                    continue;
                }
                ++context.picker_stats.checking_captures_exempted;
            }
        } else if (!checked &&
                   (m.has_flag(MoveFlag::Promotion) || m.has_flag(MoveFlag::EnPassant))) {
            ++context.picker_stats.promotions_ep_exempted;
        }

        int score = 0;
        if (m.has_flag(MoveFlag::Capture) || m.has_flag(MoveFlag::EnPassant)) {
            const Piece victim = m.has_flag(MoveFlag::EnPassant)
                ? make_piece(opposite(position.side_to_move()), PieceType::Pawn)
                : position.piece_on(m.to());
            const Piece attacker = position.piece_on(m.from());
            score = victim_value(victim) * 16 - victim_value(attacker);
        }
        if (m.has_flag(MoveFlag::Promotion)) {
            score += 80'000 + victim_value(make_piece(position.side_to_move(), m.promotion()));
        }
        q_buffer[static_cast<std::size_t>(q_count++)] = {score, m};
    }

    if (pruned_this_node) {
        ++context.picker_stats.qnodes_with_see_pruning;
    }
    context.picker_stats.tactical_moves_generated += q_count;
    std::sort(q_buffer.begin(), q_buffer.begin() + q_count,
        [](const auto& a, const auto& b) { return a.first > b.first; });

    int legal_count = 0;
    for (int i = 0; i < q_count; ++i) {
        const Move move = q_buffer[static_cast<std::size_t>(i)].second;
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
        table_.prefetch(position.key());
        PvLine child_pv;
        const int score = -quiescence(position, -beta, -alpha, ply + 1, context, child_pv);
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
            pv.prepend(move, child_pv);
            if (alpha >= beta) {
                // Tactical beta cutoff — store as lower bound
                table_.store(position.key(), best_move, best_score, 0, Bound::Lower, ply,
                             position.rule50(), static_eval, false);
                return best_score;
            }
        }
    }

    if (checked && legal_count == 0) {
        return -search_mate_score + ply;
    }

    // Store qsearch result in TT
    if (legal_count > 0 || !checked) {
        Bound bound = Bound::Exact;
        if (best_score <= original_alpha) {
            bound = Bound::Upper;
        } else if (best_score >= beta) {
            bound = Bound::Lower;
        }
        table_.store(position.key(), best_move, best_score, 0, bound, ply,
                     position.rule50(), static_eval, false);
    }

    return best_score;
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
               (context.nodes & (BulletTimeManager::clock_poll_interval(
                   context.limits.regime) - 1U)) == 0 &&
               std::chrono::steady_clock::now() - context.start >= context.limits.move_time) {
        context.stopped = true;
    }
    return context.stopped;
}

bool Searcher::consume_node(Context& context) const {
    if (context.limits.shared_node_budget) {
        if (context.local_node_budget == 0) {
            constexpr std::uint64_t chunk_size = 1024;
            std::uint64_t old =
                context.limits.shared_node_budget->load(std::memory_order_relaxed);
            while (old != 0) {
                const std::uint64_t take = old < chunk_size ? old : chunk_size;
                if (context.limits.shared_node_budget->compare_exchange_weak(
                        old, old - take, std::memory_order_relaxed, std::memory_order_relaxed)) {
                    context.local_node_budget = take;
                    break;
                }
            }
            if (old == 0) {
                context.stopped = true;
                return false;
            }
        }
        --context.local_node_budget;
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

int Searcher::maximum_ply_score(Position& position, int ply) const {
    if (!in_check(position)) {
        return evaluate_position(position);
    }
    MoveList evasions;
    generate_legal(position, evasions);
    return evasions.empty() ? -search_mate_score + ply : 0;
}

bool Searcher::is_repetition(const Context& context, std::uint64_t key) {
    return std::count(context.keys.begin(), context.keys.end(), key) >= 3;
}

}  // namespace blaze
