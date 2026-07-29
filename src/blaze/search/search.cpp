#include "blaze/search/search.h"

#include "blaze/core/movegen.h"
#include "blaze/eval/classical.h"
#include "blaze/search/see.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace blaze {
namespace {

constexpr int infinity = search_mate_score + 1;
constexpr int maximum_ply = 128;
constexpr int maximum_extensions = 2;
constexpr int history_max = 32'000;
constexpr int history_min = -32'000;
constexpr int correction_limit = 16'384;
constexpr int decisive_margin = 512;

bool decisive_score(int score) {
    return score >= search_mate_threshold - decisive_margin ||
           score <= -search_mate_threshold + decisive_margin;
}

bool same_move(Move left, Move right) {
    return left == right ||
        (left.from() == right.from() && left.to() == right.to() &&
         left.promotion() == right.promotion());
}

void canonicalize_tt_move(const MoveList& moves, Move& tt_move) {
    if (!tt_move.is_valid()) return;
    for (const Move move : moves) {
        if (same_move(move, tt_move)) {
            tt_move = move;
            return;
        }
    }
    tt_move = Move{};
}

std::uint64_t mix64(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::uint64_t pieces_of_type(const Position& position, PieceType type) {
    return static_cast<std::uint64_t>(position.pieces(Color::White, type)) ^
           std::rotl(static_cast<std::uint64_t>(position.pieces(Color::Black, type)), 29);
}

std::size_t correction_index(std::uint64_t value) {
    return static_cast<std::size_t>(mix64(value) & 16'383U);
}

int base_lmr(int depth, int move_count) {
    const double reduction = 0.65 + std::log(static_cast<double>(std::max(1, depth))) *
        std::log(static_cast<double>(std::max(1, move_count))) / 2.30;
    return std::max(0, static_cast<int>(reduction));
}

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

NnueThreadState* Searcher::prepare_nnue(const Position& position) {
    if (network_ == nullptr) return nullptr;
    if (!worker_nnue_.has_value()) {
        worker_nnue_.emplace(network_->make_thread_state());
    }
    worker_nnue_->reset(position);
    return &*worker_nnue_;
}

void Searcher::reset_task_heuristics() {
    eval_cache_ = {};
    countermoves_ = {};
    history_ = {};
    capture_history_ = {};
    pawn_history_ = {};
    continuation_history_ = {};
    low_ply_history_ = {};
    threat_history_ = {};
    correction_history_ = {};
    per_piece_history_ = {};
    root_state_.clear();
    previous_root_move_ = Move{};
    root_stability_ = 0;
}

int update_history(int value, int bonus) {
    bonus = std::clamp(bonus, history_min, history_max);
    value += bonus - value * std::abs(bonus) / history_max;
    return std::clamp(value, history_min, history_max);
}

bool quiet_move(Move move) {
    return !move.has_flag(MoveFlag::Capture) &&
           !move.has_flag(MoveFlag::EnPassant) &&
           !move.has_flag(MoveFlag::Promotion);
}

SearchResult Searcher::search(
    Position position,
    const SearchLimits& limits,
    const std::atomic<bool>* external_stop,
    const std::vector<std::uint64_t>& prior_keys) {
    std::lock_guard<std::mutex> lock(search_mutex_);
    const auto start = std::chrono::steady_clock::now();
    if (limits.threads > 1) return search_parallel(position, limits, external_stop, prior_keys, start);
    return search_single(position, limits, external_stop, prior_keys, start, 0, true);
}

SearchResult Searcher::search_single(
    Position position,
    const SearchLimits& limits,
    const std::atomic<bool>* external_stop,
    const std::vector<std::uint64_t>& prior_keys,
    std::chrono::steady_clock::time_point start,
    unsigned worker_id,
    bool bump_generation) {
    age_histories();
    eval_cache_ = {};
    root_state_.clear();
    previous_root_move_ = Move{};
    root_stability_ = 0;

    MoveList generated_moves;
    generate_legal(position, generated_moves);
    MoveList legal_moves;
    if (limits.search_moves.empty()) {
        legal_moves = generated_moves;
    } else {
        for (const Move requested : limits.search_moves) {
            for (const Move legal : generated_moves) {
                if (same_move(legal, requested)) {
                    bool duplicate = false;
                    for (const Move added : legal_moves) duplicate |= same_move(added, legal);
                    if (!duplicate && legal_moves.size() < MoveList::capacity) legal_moves.push(legal);
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
    context.nnue = prepare_nnue(position);
    context.limits = limits;
    for (auto& frame : context.stack) frame = SearchStackEntry{};
#ifndef NDEBUG
    context.limits.maximum_ply = std::clamp(
        context.limits.maximum_ply,
        1,
        maximum_ply);
#endif
    context.external_stop = external_stop;
    context.start = start;
    context.worker_id = worker_id;
    context.restricted_root = !limits.search_moves.empty();
    context.keys = prior_keys;
    context.keys.push_back(position.key());
    context.root_moves.assign(legal_moves.begin(), legal_moves.end());

    if (should_stop(context)) {
        result.stopped = true;
        return result;
    }

    if (bump_generation) table_.new_search();
    const int maximum_depth = limits.nodes > 0
        ? maximum_ply - 1
        : std::clamp(limits.depth > 0 ? limits.depth : 64, 1, maximum_ply - 1);
    for (const Move move : context.root_moves) root_state_.push_back(RootMoveState{move});
    auto state_for = [this](Move move) -> RootMoveState& {
        return *std::find_if(root_state_.begin(), root_state_.end(),
            [move](const RootMoveState& state) { return same_move(state.move, move); });
    };
    for (int depth = 1; depth <= maximum_depth; ++depth) {
        if (depth >= 6) {
          std::stable_sort(context.root_moves.begin(), context.root_moves.end(),
            [&](Move left, Move right) {
                const RootMoveState& left_state = state_for(left);
                const RootMoveState& right_state = state_for(right);
                const int left_score = left_state.score;
                const int right_score = right_state.score;
                if (left_score != right_score) return left_score > right_score;
                if (left_state.effort != right_state.effort) return left_state.effort > right_state.effort;
                return ((static_cast<unsigned>(left.raw()) + 37U * worker_id) & 0xffffU) <
                       ((static_cast<unsigned>(right.raw()) + 37U * worker_id) & 0xffffU);
            });
        }
        PvLine pv;
        int alpha = -infinity;
        int beta = infinity;
        int delta = infinity;
        if (depth >= 6 && result.depth >= 5) {
            delta = std::clamp(24 + std::abs(result.score) / 128 -
                std::min(root_stability_ * 2, 10) + static_cast<int>(worker_id % 5), 12, 96);
            alpha = std::max(-infinity, result.score - delta);
            beta = std::min(infinity, result.score + delta);
        }
        int score = result.score;
        while (true) {
            pv.clear();
            score = negamax<NodeType::Root>(position, depth, alpha, beta, 0, context, pv);
            if (context.stopped || (score > alpha && score < beta)) break;
            if (score >= search_mate_threshold || score <= -search_mate_threshold) {
                alpha = -infinity;
                beta = infinity;
                delta = infinity;
                continue;
            }
            if (delta == infinity) {
                alpha = -infinity;
                beta = infinity;
                continue;
            }
            if (score <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = std::max(-infinity, score - delta);
            } else {
                alpha = std::max(alpha, beta - delta / 2);
                beta = std::min(infinity, score + delta);
            }
            delta = std::min(infinity / 2, delta + delta / 2 + 8);
            if (alpha <= -infinity + 1 && beta >= infinity - 1) {
                alpha = -infinity;
                beta = infinity;
                delta = infinity;
            }
        }
        if (context.stopped) {
            break;
        }
        if (!pv.empty()) {
            result.best_move = pv.front();
            result.pv.assign(pv.span().begin(), pv.span().end());
            RootMoveState& root_state = state_for(result.best_move);
            root_state.score = score;
            root_stability_ = result.best_move == previous_root_move_
                ? root_stability_ + 1
                : 0;
            previous_root_move_ = result.best_move;
        }
        if (score >= search_mate_threshold) {
            const Bitboard enemy_king = position.pieces(
                opposite(position.side_to_move()), PieceType::King);
            if (enemy_king != 0) {
                const Square king_square = static_cast<Square>(std::countr_zero(enemy_king));
                auto utility = [&position, king_square](Move move) {
                    return std::tuple{
                        type_of(position.piece_on(move.from())) == PieceType::Queen ? 0 : 1,
                        std::abs(file_of(move.to()) - file_of(king_square)),
                        std::abs(file_of(move.to()) * 2 - 7) +
                            std::abs(rank_of(move.to()) * 2 - 7)};
                };
                for (const RootMoveState& candidate : root_state_) {
                    if (candidate.score == score &&
                        utility(candidate.move) < utility(result.best_move)) {
                        result.best_move = candidate.move;
                        result.pv = {candidate.move};
                    }
                }
            }
        }
        const int score_delta = std::abs(score - result.score);
        result.score = score;
        result.depth = depth;
        if (score >= search_mate_threshold || score <= -search_mate_threshold) {
            break;
        }
        if (limits.target_time.count() > 0 &&
            [&] {
                double soft_scale = root_stability_ >= 4 ? 0.65
                    : root_stability_ >= 2 ? 0.80
                    : 1.0;
                if (score_delta >= 50) soft_scale *= 1.20;
                const auto soft_limit = std::min(
                    limits.move_time,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        limits.target_time * soft_scale));
                return std::chrono::steady_clock::now() - context.start >= soft_limit;
            }()) {
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
    result.stopped = context.stopped || limits.nodes > 0;
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
    NnueThreadState* nnue = prepare_nnue(position);
    return search_window(
        std::move(position),
        limits,
        depth,
        0,
        Move{},
        0,
        alpha,
        beta,
        nnue,
        nullptr,
        {},
        std::chrono::steady_clock::now());
}
#endif

SearchResult Searcher::search_parallel(
    Position position,
    const SearchLimits& limits,
    const std::atomic<bool>* external_stop,
    const std::vector<std::uint64_t>& prior_keys,
    std::chrono::steady_clock::time_point start) {
    if (limits.threads <= 1) {
        return search_single(position, limits, external_stop, prior_keys, start, 0, true);
    }

    const unsigned worker_count = static_cast<unsigned>(std::clamp(limits.threads, 1, 64));
    workers_.reserve(worker_count);
    while (workers_.size() < worker_count) {
        auto worker = std::make_unique<Searcher>(table_, network_);
        static_cast<void>(worker->prepare_nnue(position));
        workers_.push_back(std::move(worker));
    }

    table_.new_search();

    const auto shared_node_budget = limits.nodes > 0
        ? (limits.shared_node_budget ? limits.shared_node_budget : std::make_shared<std::atomic<std::uint64_t>>(limits.nodes))
        : std::shared_ptr<std::atomic<std::uint64_t>>{};

    std::vector<SearchResult> worker_results(worker_count);
    std::vector<std::thread> thread_pool;
    thread_pool.reserve(worker_count);

    for (unsigned w = 0; w < worker_count; ++w) {
        thread_pool.emplace_back([&, w] {
            SearchLimits child_limits = limits;
            child_limits.threads = 1;
            child_limits.shared_node_budget = shared_node_budget;
            worker_results[w] = workers_[w]->search_single(
                position, child_limits, external_stop, prior_keys, start, w, false);
        });
    }

    for (auto& thread : thread_pool) {
        thread.join();
    }

    struct Vote { Move move; std::int64_t weight = 0; };
    std::vector<Vote> votes;
    for (const SearchResult& candidate : worker_results) {
        if (!candidate.best_move.is_valid() || candidate.depth <= 0) continue;
        auto vote = std::find_if(votes.begin(), votes.end(), [&](const Vote& item) {
            return same_move(item.move, candidate.best_move);
        });
        const std::int64_t depth = candidate.depth;
        const std::int64_t weight = (depth + 2) * (depth + 2) * 32 +
            (candidate.stopped ? 0 : 256) +
            (candidate.score >= search_mate_threshold ? 1'000'000 : 0);
        if (vote == votes.end()) votes.push_back(Vote{candidate.best_move, weight});
        else vote->weight += weight;
    }
    const Move voted_move = votes.empty() ? worker_results[0].best_move :
        std::max_element(votes.begin(), votes.end(), [](const Vote& left, const Vote& right) {
            return left.weight < right.weight;
        })->move;

    SearchResult best_result = worker_results[0];
    bool found = false;
    for (const SearchResult& candidate : worker_results) {
        if (!same_move(candidate.best_move, voted_move)) continue;
        if (!found || (!candidate.stopped && best_result.stopped) ||
            (candidate.stopped == best_result.stopped && candidate.depth > best_result.depth) ||
            (candidate.stopped == best_result.stopped && candidate.depth == best_result.depth &&
             candidate.score > best_result.score)) {
            best_result = candidate;
            found = true;
        }
    }

    if (shared_node_budget && limits.nodes > 0) {
        const std::uint64_t remaining = shared_node_budget->load(std::memory_order_relaxed);
        best_result.nodes = limits.nodes > remaining ? limits.nodes - remaining : limits.nodes;
    } else {
        std::uint64_t total_nodes = 0;
        for (unsigned w = 0; w < worker_count; ++w) {
            total_nodes += worker_results[w].nodes;
        }
        best_result.nodes = total_nodes;
    }

    return best_result;
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
    NnueThreadState* nnue,
    const std::atomic<bool>* external_stop,
    const std::vector<std::uint64_t>& prior_keys,
    std::chrono::steady_clock::time_point start) {
    Context context;
    context.nnue = nnue;
    context.limits = limits;
    for (auto& frame : context.stack) frame = SearchStackEntry{};
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
            for (const Move requested : context.root_moves) {
                for (const Move candidate : legal_moves) {
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
        return maximum_ply_score(position, ply, context);
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
    const bool selective_window = !decisive_score(alpha) && !decisive_score(beta);

    auto& frame = context.stack[static_cast<std::size_t>(ply)];
    const bool excluded_search = frame.excluded_move.is_valid();
    const Move previous_move = frame.current_move;
    Move tt_move;
    int raw_static_eval = tt_no_static_evaluation;
    bool rule50_safe = false;
    const auto tt_hit = table_.probe(position.key(), ply, position.rule50());
    if (tt_hit) {
        tt_move = tt_hit->move;
        rule50_safe = tt_hit->rule50 == std::min(position.rule50(), 100);
        if (rule50_safe) raw_static_eval = tt_hit->static_evaluation;
        if constexpr (node_type == NodeType::NonPV) {
          if (!excluded_search && rule50_safe && tt_hit->depth >= depth) {
            if (tt_hit->bound == Bound::Exact ||
                (tt_hit->bound == Bound::Lower && tt_hit->score >= beta) ||
                (tt_hit->bound == Bound::Upper && tt_hit->score <= alpha)) {
                return tt_hit->score;
            }
          }
        }
        if constexpr (node_type == NodeType::PV) {
            if (!excluded_search && rule50_safe && tt_hit->depth >= depth &&
                (tt_hit->bound == Bound::Exact ||
                 (tt_hit->bound == Bound::Lower && tt_hit->score >= beta) ||
                 (tt_hit->bound == Bound::Upper && tt_hit->score <= alpha))) {
                return tt_hit->score;
            }
        }
    }
    canonicalize_tt_move(legal_moves, tt_move);
    if constexpr (node_type == NodeType::Root) {
        Bitboard own = 0;
        Bitboard enemy = 0;
        for (int piece = static_cast<int>(PieceType::Pawn);
             piece <= static_cast<int>(PieceType::King); ++piece) {
            own |= position.pieces(position.side_to_move(), static_cast<PieceType>(piece));
            enemy |= position.pieces(opposite(position.side_to_move()),
                                     static_cast<PieceType>(piece));
        }
        if (std::popcount(static_cast<std::uint64_t>(own)) == 2 &&
            std::popcount(static_cast<std::uint64_t>(enemy)) == 1 &&
            position.pieces(position.side_to_move(), PieceType::Queen) != 0) {
            const Square king_square = static_cast<Square>(std::countr_zero(enemy));
            Move preferred;
            std::pair<int, int> preferred_utility{64, 64};
            for (const Move move : legal_moves) {
                if (type_of(position.piece_on(move.from())) != PieceType::Queen) continue;
                const Color moving_side = position.side_to_move();
                StateInfo state;
                if (!position.make_move(move, state, false)) continue;
                const bool checking = king_is_safe_after_move(position, moving_side) &&
                                      in_check(position);
                position.unmake_move(move, state);
                if (!checking) continue;
                const std::pair utility{
                    std::abs(file_of(move.to()) - file_of(king_square)),
                    std::abs(file_of(move.to()) * 2 - 7) +
                        std::abs(rank_of(move.to()) * 2 - 7)};
                if (!preferred.is_valid() || utility < preferred_utility ||
                    (utility == preferred_utility && move.raw() < preferred.raw())) {
                    preferred = move;
                    preferred_utility = utility;
                }
            }
            if (preferred.is_valid()) tt_move = preferred;
        }
    }

    int static_eval = tt_no_static_evaluation;
    int search_eval = tt_no_static_evaluation;
    if (!checked) {
        if (raw_static_eval == tt_no_static_evaluation || depth >= 12) {
            raw_static_eval = evaluate_position(position, context);
        }
        static_eval = std::clamp(raw_static_eval + correction_value(position, previous_move),
            -search_mate_threshold + 1, search_mate_threshold - 1);
        search_eval = static_eval;
        if (tt_hit && rule50_safe && !decisive_score(tt_hit->score)) {
            if (tt_hit->bound == Bound::Exact ||
                (tt_hit->bound == Bound::Lower && tt_hit->score > search_eval) ||
                (tt_hit->bound == Bound::Upper && tt_hit->score < search_eval)) {
                search_eval = tt_hit->score;
            }
        }
        frame.static_evaluation = static_eval;
    } else {
        frame.static_evaluation = tt_no_static_evaluation;
    }

    bool improving = false;
    if (!checked && ply >= 2) {
        const int prior = context.stack[static_cast<std::size_t>(ply - 2)].static_evaluation;
        improving = prior == tt_no_static_evaluation || static_eval > prior;
    }
    bool opponent_worsening = false;
    if (!checked && ply >= 1) {
        const int opponent = context.stack[static_cast<std::size_t>(ply - 1)].static_evaluation;
        opponent_worsening = opponent != tt_no_static_evaluation && static_eval > -opponent;
    }

    if constexpr (node_type == NodeType::NonPV) {
        const int rfp_margin = 65 + 75 * depth + 18 * depth * depth -
            (improving ? 70 : 0) - (opponent_worsening ? 20 : 0);
        if (!checked && !excluded_search && !decisive_score(beta) && depth <= 9 &&
            search_eval - rfp_margin >= beta) {
            return search_eval - rfp_margin / 2;
        }
        if (!checked && !excluded_search && !decisive_score(alpha) && depth <= 3 &&
            search_eval + 180 + 150 * depth * depth <= alpha) {
            PvLine razor_pv;
            const int razor = quiescence(position, alpha, beta, ply, context, razor_pv);
            if (!context.stopped && razor <= alpha) return razor;
        }
        const bool null_enabled =
#ifndef NDEBUG
            context.limits.enable_null_move;
#else
            true;
#endif
        if (null_enabled && allow_null && depth >= 3 && !checked && !excluded_search &&
            selective_window && position.rule50() < 90 &&
            search_eval >= beta &&
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
            const int eval_term = std::clamp((search_eval - beta) / 180, 0, 3);
            const int reduction = std::min(depth - 1, 3 + depth / 4 + eval_term);
            StateInfo null_state;
            position.make_null(null_state, context.nnue != nullptr);
            if (context.nnue) context.nnue->push(position, null_state);
            table_.prefetch(position.key());
            PvLine null_pv;
            auto& child = context.stack[static_cast<std::size_t>(ply + 1)];
            child = SearchStackEntry{};
            child.extension_count = frame.extension_count;
            const int null_score = -negamax<NodeType::NonPV>(
                position,
                depth - 1 - reduction,
                -beta,
                -beta + 1,
                ply + 1,
                context,
                null_pv,
                false);
            if (context.nnue) context.nnue->pop();
            position.unmake_null(null_state);
            if (context.stopped) {
                return 0;
            }
            if (null_score >= beta && !decisive_score(null_score)) {
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
    if constexpr (node_type == NodeType::NonPV) {
      if (probcut_enabled && selective_window && depth >= 3 && !checked && !excluded_search) {
        const int probcut_margin = 120 + depth * 10;
        const int probcut_depth = std::max(1, depth - (depth <= 5 ? 2 : 4));
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
            if (!position.make_move(move, state, context.nnue != nullptr)) continue;
            if (!king_is_safe_after_move(position, moving_side)) {
                position.unmake_move(move, state);
                continue;
            }
            if (context.nnue) context.nnue->push(position, state);
#ifndef NDEBUG
            if (in_check(position)) {
                ++context.probcut_legal_checks;
            }
#endif
            context.keys.push_back(position.key());
            table_.prefetch(position.key());
            auto& child = context.stack[static_cast<std::size_t>(ply + 1)];
            child = SearchStackEntry{};
            child.current_move = move;
            child.extension_count = frame.extension_count;
            PvLine probe_pv;
            const int probe_score = -negamax<NodeType::NonPV>(
                position,
                probcut_depth,
                -beta - probcut_margin,
                -beta,
                ply + 1,
                context,
                probe_pv);
            context.keys.pop_back();
            if (context.nnue) context.nnue->pop();
            position.unmake_move(move, state);
            if (context.stopped) return 0;
            if (probe_score >= beta + probcut_margin) {
                table_.store(position.key(), move, probe_score, probcut_depth + 1,
                    Bound::Lower, ply, position.rule50(), raw_static_eval, false);
                return probe_score - probcut_margin;
            }
        }
      }
    }

    if (!excluded_search && !tt_move.is_valid() && depth >= 6 &&
        (pv_node || (tt_hit && rule50_safe && tt_hit->bound == Bound::Lower))) --depth;

    int singular_extension = 0;
    if constexpr (node_type == NodeType::NonPV) {
        if (!checked && tt_hit && rule50_safe && tt_move.is_valid() && depth >= 7 &&
            (tt_hit->bound == Bound::Lower || tt_hit->bound == Bound::Exact) &&
            tt_hit->depth >= depth - 3 && !decisive_score(tt_hit->score) &&
            !excluded_search) {
            const int singular_beta = tt_hit->score - (18 + 2 * depth);
            const Move saved_excluded = frame.excluded_move;
            frame.excluded_move = tt_move;
            PvLine singular_pv;
            const int singular_score = negamax<NodeType::NonPV>(
                position,
                std::max(1, (depth - 1) / 2),
                singular_beta - 1,
                singular_beta,
                ply,
                context,
                singular_pv,
                false);
            frame.excluded_move = saved_excluded;
            if (context.stopped) return 0;
            if (singular_score < singular_beta) {
                singular_extension = singular_score < singular_beta - 105 && depth >= 10 ? 2 : 1;
            } else if (singular_score >= beta && tt_hit->score >= beta &&
                       !decisive_score(singular_score)) {
                return singular_score;
            } else if (tt_hit->score >= beta) {
                singular_extension = -1;
            }
        }
    }

    const int original_alpha = alpha;
    Move best_move;
    int best_score = -infinity;
    int legal_count = 0;
    const Move counter_move = previous_move.is_valid()
        ? countermoves_[square_index(previous_move.from())][square_index(previous_move.to())]
        : Move{};

    MovePicker picker(position, legal_moves, tt_move,
                      frame.killers.data(),
                      counter_move,
                      position.side_to_move(),
                      previous_move,
                      history_[static_cast<std::size_t>(position.side_to_move())],
                      capture_history_,
                      pawn_history_,
                      continuation_history_,
                      low_ply_history_,
                      threat_history_,
                      per_piece_history_,
                      ply);

    MoveList searched_quiets;
    MoveList searched_captures;
    int move_count = 0;
    int searched_count = 0;
    bool beta_cutoff = false;
    while (true) {
        const Move move = picker.next();
        if (!move.is_valid()) break;
        if (move == context.stack[static_cast<std::size_t>(ply)].excluded_move) continue;
        ++move_count;
        const bool quiet = quiet_move(move);
        const bool tactical = !quiet;
        const auto all_pieces = [&position](PieceType type) {
            return position.pieces(Color::White, type) |
                   position.pieces(Color::Black, type);
        };
        const bool sparse_opposing_majors =
            (position.pieces(Color::White, PieceType::Rook) |
             position.pieces(Color::White, PieceType::Queen)) != 0 &&
            (position.pieces(Color::Black, PieceType::Rook) |
             position.pieces(Color::Black, PieceType::Queen)) != 0 &&
            std::popcount(static_cast<std::uint64_t>(
                all_pieces(PieceType::Knight) | all_pieces(PieceType::Bishop) |
                all_pieces(PieceType::Rook) | all_pieces(PieceType::Queen))) <= 2;
        if (move.has_flag(MoveFlag::Promotion) || move.has_flag(MoveFlag::EnPassant)) {
            ++context.picker_stats.promotions_ep_exempted;
        }
        const bool tt_selected = same_move(move, tt_move);
        const bool killer = move == frame.killers[0] || move == frame.killers[1];
        const bool counter = move == counter_move;
        const int history_score = quiet
            ? quiet_history_score(position, move, previous_move, ply)
            : 0;
        bool preprobed = false;
        bool gives_check = false;
        if constexpr (node_type == NodeType::NonPV) {
            if (selective_window && !sparse_opposing_majors && !checked && !excluded_search &&
                !tt_selected && move_count > 1 &&
                ((quiet && depth <= 10) || (tactical && depth <= 8))) {
                const Color moving_side = position.side_to_move();
                StateInfo probe_state;
                if (!position.make_move(move, probe_state, false)) continue;
                const bool legal = king_is_safe_after_move(position, moving_side);
                if (legal) gives_check = in_check(position);
                position.unmake_move(move, probe_state);
                if (!legal) continue;
                preprobed = true;
                ++legal_count;

                const bool advanced_pawn = quiet &&
                    type_of(position.piece_on(move.from())) == PieceType::Pawn &&
                    rank_of(move.to()) == (position.side_to_move() == Color::White ? 6 : 1);
                if (!gives_check && quiet && !killer && !counter && !advanced_pawn) {
                    const int lmp_limit = (improving ? 5 : 3) +
                        std::min(depth, 10) * std::min(depth, 10) / (improving ? 2 : 3);
                    if (legal_count > lmp_limit ||
                        (depth <= 7 && static_eval + 80 + 95 * depth + 35 * depth * depth <= alpha &&
                         history_score < 5'000) ||
                        (depth <= 6 && history_score < -3'500 * depth)) {
                        update_history_penalty(position, move, previous_move, depth);
                        continue;
                    }
                }
                if (!gives_check && tactical && !move.has_flag(MoveFlag::Promotion) &&
                    !move.has_flag(MoveFlag::EnPassant)) {
                    const Piece victim = position.piece_on(move.to());
                    if ((static_eval + victim_value(victim) + 140 + 120 * depth <= alpha &&
                         !see_ge(position, move, 0)) ||
                        !see_ge(position, move, -(70 * depth + (improving ? 35 : 0)))) {
                        continue;
                    }
                }
            }
        }
        const bool recaptures = previous_move.is_valid() &&
            previous_move.to() == move.to() &&
            (previous_move.has_flag(MoveFlag::Capture) ||
             previous_move.has_flag(MoveFlag::EnPassant)) &&
             (move.has_flag(MoveFlag::Capture) || move.has_flag(MoveFlag::EnPassant));
        const bool pawn_advance = quiet &&
            type_of(position.piece_on(move.from())) == PieceType::Pawn &&
            rank_of(move.to()) == (position.side_to_move() == Color::White ? 6 : 1);
        const int see_score = recaptures
            ? static_exchange_evaluation(position, move)
            : std::numeric_limits<int>::min();
        StateInfo state;
        const Color moving_side = position.side_to_move();
        if (!position.make_move(move, state, context.nnue != nullptr)) {
            continue;
        }
        if (!king_is_safe_after_move(position, moving_side)) {
            position.unmake_move(move, state);
            continue;
        }
        if (context.nnue) context.nnue->push(position, state);
        if (!preprobed) {
            ++legal_count;
            gives_check = in_check(position);
        }
        ++searched_count;
        context.keys.push_back(position.key());
        table_.prefetch(position.key());
        PvLine child_pv;
        const int used = frame.extension_count;
        int extension = same_move(move, tt_move) ? singular_extension : 0;
        if (used < maximum_extensions || extension < 0) {
            const bool selective_check = gives_check && depth >= 3
                && move_count <= 3;
            const bool sound_recapture = recaptures && depth <= 10 && see_score >= 0;
            const bool forcing_pawn = move.has_flag(MoveFlag::Promotion) || pawn_advance;
            if (selective_check || sound_recapture || forcing_pawn) ++extension;
        }
        extension = std::clamp(extension, -1, maximum_extensions - used);
        const int child_extension_count = used + std::max(0, extension);
        auto& child = context.stack[static_cast<std::size_t>(ply + 1)];
        child = SearchStackEntry{};
        child.current_move = move;
        child.extension_count = child_extension_count;
#ifndef NDEBUG
        context.maximum_extension_count = std::max(
            context.maximum_extension_count,
            child_extension_count);
#endif
        const int full_depth = depth - 1 + extension;
        int score = 0;
        const std::uint64_t nodes_before = context.nodes;
        if (searched_count == 1) {
            if constexpr (pv_node) {
                score = -negamax<NodeType::PV>(
                    position, full_depth, -beta, -alpha, ply + 1, context, child_pv);
            } else {
                score = -negamax<NodeType::NonPV>(
                    position, full_depth, -beta, -alpha, ply + 1, context, child_pv);
            }
        } else {
            int reduction = 0;
            if (selective_window && !checked && !sparse_opposing_majors &&
                depth >= 2 && full_depth > 0 && !tt_selected) {
                reduction = base_lmr(depth, move_count);
                if constexpr (!pv_node) ++reduction;
                if (!improving) ++reduction;
                if (!tt_move.is_valid()) ++reduction;
                if (tactical) --reduction;
                if (gives_check || killer || counter || opponent_worsening) --reduction;
                if (history_score > 6'000) --reduction;
                if (history_score < -4'000) ++reduction;
                reduction -= std::max(0, extension);
                reduction = std::clamp(reduction, 0, std::max(0, full_depth - 1));
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
                int research_depth = full_depth;
                if (score < best_score + 12) --research_depth;
                research_depth = std::clamp(research_depth, 0, full_depth);
                score = -negamax<NodeType::NonPV>(
                    position,
                    research_depth,
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
        if (context.nnue) context.nnue->pop();
        position.unmake_move(move, state);

        if constexpr (node_type == NodeType::Root) {
            auto root = std::find_if(root_state_.begin(), root_state_.end(),
                [move](const RootMoveState& item) { return same_move(item.move, move); });
            if (root != root_state_.end()) {
                root->score = score;
                root->effort += context.nodes - nodes_before;
            }
        }

        if (context.stopped) {
            return 0;
        }
        auto root_mate_utility = [&position](Move candidate) {
            const Bitboard king = position.pieces(
                opposite(position.side_to_move()), PieceType::King);
            if (king == 0) return std::tuple{1, 64, 64};
            const Square square = static_cast<Square>(std::countr_zero(king));
            return std::tuple{
                type_of(position.piece_on(candidate.from())) == PieceType::Queen ? 0 : 1,
                std::abs(file_of(candidate.to()) - file_of(square)),
                std::abs(file_of(candidate.to()) * 2 - 7) +
                    std::abs(rank_of(candidate.to()) * 2 - 7)};
        };
        const bool prefer_equal_root_mate = node_type == NodeType::Root &&
            score == best_score && score >= search_mate_threshold &&
            (!best_move.is_valid() ||
             root_mate_utility(move) < root_mate_utility(best_move));
        if (score > best_score || prefer_equal_root_mate) {
            best_score = score;
            best_move = move;
        }
        if (score > alpha || prefer_equal_root_mate) {
            alpha = score;
            pv.prepend(move, child_pv);
        }
        if (alpha >= beta) {
            beta_cutoff = true;
            picker.on_cutoff(move_count);
            const bool quiet = !move.has_flag(MoveFlag::Capture) &&
                !move.has_flag(MoveFlag::EnPassant) && !move.has_flag(MoveFlag::Promotion);
            update_history_tables(position, move, previous_move, depth, ply);
            if (previous_move.is_valid()) {
                const int bonus = std::min(1'000, depth * depth * 16);
                threat_history_[square_index(previous_move.to())][square_index(move.to())]
                    = update_history(
                        threat_history_[square_index(previous_move.to())][square_index(move.to())],
                        bonus);
            }
            if (quiet) {
                auto& killers = frame.killers;
                if (move != killers[0]) {
                    killers[1] = killers[0];
                    killers[0] = move;
                }
            }
            if (!excluded_search) {
                for (const Move failed : searched_quiets) {
                    if (!same_move(failed, move)) update_history_penalty(
                        position, failed, previous_move, depth);
                }
                for (const Move failed : searched_captures) {
                    if (!same_move(failed, move)) update_history_penalty(
                        position, failed, previous_move, depth);
                }
            }
            break;
        }
        if (quiet) searched_quiets.push(move);
        else searched_captures.push(move);
        if constexpr (node_type == NodeType::Root) {
            if (best_score >= search_mate_threshold &&
                root_mate_utility(best_move) <= std::tuple{0, 0, 5}) break;
        }
    }

    context.picker_stats.accumulate(picker.collect_stats());

    if (legal_count == 0) {
        if (excluded_search) return original_alpha;
        return checked ? -search_mate_score + ply : 0;
    }
    if (searched_count == 0) best_score = alpha;

    if (!excluded_search && !beta_cutoff && best_move.is_valid() &&
        best_score > original_alpha) {
        const int feedback_depth = std::max(1, depth / 2);
        update_history_tables(position, best_move, previous_move, feedback_depth, ply);
        for (const Move failed : searched_quiets) {
            if (!same_move(failed, best_move)) update_history_penalty(
                position, failed, previous_move, feedback_depth);
        }
        for (const Move failed : searched_captures) {
            if (!same_move(failed, best_move)) update_history_penalty(
                position, failed, previous_move, feedback_depth);
        }
    }

    Bound bound = Bound::Upper;
    if (best_score >= beta) bound = Bound::Lower;
    else if (pv_node && best_move.is_valid() && best_score > original_alpha) bound = Bound::Exact;

    const bool reliable_correction = bound == Bound::Exact ||
        (bound == Bound::Lower && best_score > raw_static_eval) ||
        (bound == Bound::Upper && best_score < raw_static_eval);
    if (!checked && !excluded_search && reliable_correction &&
        (!best_move.is_valid() || quiet_move(best_move)) &&
        !decisive_score(best_score)) {
        update_correction(position, previous_move, raw_static_eval, best_score, depth);
    }
    if (!excluded_search && !(node_type == NodeType::Root && context.restricted_root)) {
        table_.store(position.key(), best_move, best_score, depth, bound, ply,
            position.rule50(), raw_static_eval, pv_node);
    }
    return best_score;
}

void Searcher::update_history_tables(const Position& position,
                                     Move move,
                                     Move previous_move,
                                     int depth,
                                     int ply) {
    const int fi = square_index(move.from());
    const int ti = square_index(move.to());
    const Color us = position.side_to_move();
    const int color_index = static_cast<int>(us);

    const int bonus = std::min(2'000, depth * depth * 32);
    const bool quiet = !move.has_flag(MoveFlag::Capture) &&
        !move.has_flag(MoveFlag::EnPassant) && !move.has_flag(MoveFlag::Promotion);
    if (quiet) {
        history_[color_index][fi][ti] = update_history(history_[color_index][fi][ti], bonus);
        if (previous_move.is_valid()) {
            continuation_history_[square_index(previous_move.to())][ti] = update_history(
                continuation_history_[square_index(previous_move.to())][ti], bonus);
        }
        const std::size_t piece = static_cast<std::size_t>(
            type_of(position.piece_on(move.from())));
        per_piece_history_[piece][ti] = update_history(per_piece_history_[piece][ti], bonus);
        if (type_of(position.piece_on(move.from())) == PieceType::Pawn) {
            pawn_history_[fi][ti] = update_history(pawn_history_[fi][ti], bonus);
        }
        if (ply < 4) {
            low_ply_history_[fi][ti] = update_history(low_ply_history_[fi][ti], bonus);
        }
    } else {
        const Piece attacker = position.piece_on(move.from());
        const Piece victim = move.has_flag(MoveFlag::EnPassant)
            ? make_piece(opposite(us), PieceType::Pawn)
            : position.piece_on(move.to());
        capture_history_[static_cast<std::size_t>(type_of(attacker))]
            [static_cast<std::size_t>(type_of(victim))][ti] = update_history(
                capture_history_[static_cast<std::size_t>(type_of(attacker))]
                    [static_cast<std::size_t>(type_of(victim))][ti], bonus);
    }

    if (previous_move.is_valid()) {
        countermoves_[square_index(previous_move.from())]
            [square_index(previous_move.to())] = move;
    }
}

void Searcher::update_history_penalty(const Position& position,
                                      Move move,
                                      Move previous_move,
                                      int depth) {
    const int fi = square_index(move.from());
    const int ti = square_index(move.to());
    const Color us = position.side_to_move();
    const int color_index = static_cast<int>(us);
    const int penalty = std::min(1'000, std::max(16, depth * depth * 16));
    const bool quiet = !move.has_flag(MoveFlag::Capture) &&
        !move.has_flag(MoveFlag::EnPassant) && !move.has_flag(MoveFlag::Promotion);
    if (quiet) {
        history_[color_index][fi][ti] = update_history(history_[color_index][fi][ti], -penalty);
        if (previous_move.is_valid()) {
            continuation_history_[square_index(previous_move.to())][ti] = update_history(
                continuation_history_[square_index(previous_move.to())][ti], -penalty);
            threat_history_[square_index(previous_move.to())][ti] = update_history(
                threat_history_[square_index(previous_move.to())][ti], -penalty);
        }
        per_piece_history_[static_cast<std::size_t>(type_of(position.piece_on(move.from())))][ti]
            = update_history(
                per_piece_history_[static_cast<std::size_t>(type_of(position.piece_on(move.from())))][ti],
                -penalty);
        if (type_of(position.piece_on(move.from())) == PieceType::Pawn) {
            pawn_history_[fi][ti] = update_history(pawn_history_[fi][ti], -penalty);
        }
    } else {
        const Piece attacker = position.piece_on(move.from());
        const Piece victim = move.has_flag(MoveFlag::EnPassant)
            ? make_piece(opposite(us), PieceType::Pawn)
            : position.piece_on(move.to());
        capture_history_[static_cast<std::size_t>(type_of(attacker))]
            [static_cast<std::size_t>(type_of(victim))][ti] = update_history(
                capture_history_[static_cast<std::size_t>(type_of(attacker))]
                    [static_cast<std::size_t>(type_of(victim))][ti], -penalty);
    }
}

int Searcher::quiet_history_score(
    const Position& position, Move move, Move previous_move, int ply) const {
    const int from = square_index(move.from());
    const int to = square_index(move.to());
    int score = history_[static_cast<std::size_t>(position.side_to_move())][from][to];
    if (previous_move.is_valid()) {
        score += continuation_history_[square_index(previous_move.to())][to];
        score += threat_history_[square_index(previous_move.to())][to];
    }
    score += per_piece_history_[static_cast<std::size_t>(
        type_of(position.piece_on(move.from())))][to];
    if (type_of(position.piece_on(move.from())) == PieceType::Pawn) {
        score += pawn_history_[from][to];
    }
    if (ply < 4) score += low_ply_history_[from][to];
    return score;
}

int Searcher::correction_value(const Position& position, Move previous_move) const {
    const std::size_t side = static_cast<std::size_t>(position.side_to_move());
    const auto index = [](std::uint64_t value) { return correction_index(value); };
    const std::size_t pawn = index(pieces_of_type(position, PieceType::Pawn));
    const std::size_t minor = index(pieces_of_type(position, PieceType::Knight) ^
        std::rotl(pieces_of_type(position, PieceType::Bishop), 17));
    const std::size_t major = index(pieces_of_type(position, PieceType::Rook) ^
        std::rotl(pieces_of_type(position, PieceType::Queen), 23));
    const std::size_t non_pawn = index(pieces_of_type(position, PieceType::Knight) ^
        std::rotl(pieces_of_type(position, PieceType::Bishop), 11) ^
        std::rotl(pieces_of_type(position, PieceType::Rook), 22) ^
        std::rotl(pieces_of_type(position, PieceType::Queen), 37));
    int sum = correction_history_.pawn[side][pawn] +
        correction_history_.minor[side][minor] +
        correction_history_.major[side][major] +
        correction_history_.non_pawn[side][non_pawn];
    if (previous_move.is_valid()) {
        const std::size_t continuation = static_cast<std::size_t>(
            square_index(previous_move.from()) * 64 + square_index(previous_move.to()));
        sum += correction_history_.continuation[side][continuation];
    }
    return std::clamp(sum / 128, -512, 512);
}

void Searcher::update_correction(const Position& position, Move previous_move,
                                 int raw_eval, int searched_value, int depth) {
    if (raw_eval == tt_no_static_evaluation || decisive_score(raw_eval) ||
        decisive_score(searched_value)) return;
    const int error = std::clamp(searched_value - raw_eval, -768, 768);
    const int bonus = std::clamp(error * std::max(1, depth) / 8, -2048, 2048);
    const std::size_t side = static_cast<std::size_t>(position.side_to_move());
    auto gravity = [bonus](int& value) {
        value += bonus - value * std::abs(bonus) / correction_limit;
        value = std::clamp(value, -correction_limit, correction_limit);
    };
    gravity(correction_history_.pawn[side][correction_index(
        pieces_of_type(position, PieceType::Pawn))]);
    gravity(correction_history_.minor[side][correction_index(
        pieces_of_type(position, PieceType::Knight) ^
        std::rotl(pieces_of_type(position, PieceType::Bishop), 17))]);
    gravity(correction_history_.major[side][correction_index(
        pieces_of_type(position, PieceType::Rook) ^
        std::rotl(pieces_of_type(position, PieceType::Queen), 23))]);
    gravity(correction_history_.non_pawn[side][correction_index(
        pieces_of_type(position, PieceType::Knight) ^
        std::rotl(pieces_of_type(position, PieceType::Bishop), 11) ^
        std::rotl(pieces_of_type(position, PieceType::Rook), 22) ^
        std::rotl(pieces_of_type(position, PieceType::Queen), 37))]);
    if (previous_move.is_valid()) {
        gravity(correction_history_.continuation[side][static_cast<std::size_t>(
            square_index(previous_move.from()) * 64 + square_index(previous_move.to()))]);
    }
}

void Searcher::age_histories() {
    auto age = [](auto& table, int numerator, int denominator) {
        auto visit = [&](auto&& self, auto& value) -> void {
            using T = std::remove_cv_t<std::remove_reference_t<decltype(value)>>;
            if constexpr (std::is_integral_v<T>) value = value * numerator / denominator;
            else for (auto& child : value) self(self, child);
        };
        visit(visit, table);
    };
    age(history_, 63, 64);
    age(capture_history_, 63, 64);
    age(pawn_history_, 63, 64);
    age(continuation_history_, 63, 64);
    age(low_ply_history_, 63, 64);
    age(per_piece_history_, 63, 64);
    age(threat_history_, 63, 64);
    if ((++correction_history_.searches & 7U) == 0U) {
        age(correction_history_.pawn, 255, 256);
        age(correction_history_.minor, 255, 256);
        age(correction_history_.major, 255, 256);
        age(correction_history_.non_pawn, 255, 256);
        age(correction_history_.continuation, 255, 256);
    }
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
        return maximum_ply_score(position, ply, context);
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
    const bool pv_node = beta - alpha > 1;
    const Move previous_move = context.stack[static_cast<std::size_t>(ply)].current_move;
    Move tt_move;
    int raw_static_eval = tt_no_static_evaluation;
    bool rule50_safe = false;
    if (!checked) {
        const auto tt_hit = table_.probe(position.key(), ply, position.rule50());
        if (tt_hit) {
            tt_move = tt_hit->move;
            rule50_safe = tt_hit->rule50 == std::min(position.rule50(), 100);
            if (rule50_safe) raw_static_eval = tt_hit->static_evaluation;
            if (!pv_node && rule50_safe && tt_hit->depth >= 0) {
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
    int stand_pat = -infinity;

    // Stand-pat evaluation (use TT static_eval if available)
    if (!checked) {
        if (raw_static_eval != tt_no_static_evaluation) {
            stand_pat = raw_static_eval;
        } else {
            stand_pat = evaluate_position(position, context);
            raw_static_eval = stand_pat;
        }
        stand_pat = std::clamp(stand_pat + correction_value(position, previous_move),
            -search_mate_threshold + 1, search_mate_threshold - 1);
        context.stack[static_cast<std::size_t>(ply)].static_evaluation = stand_pat;
        if (stand_pat >= beta) {
            table_.store(position.key(), Move{}, stand_pat, 0, Bound::Lower, ply,
                         position.rule50(), raw_static_eval, false);
            return stand_pat;
        }
        alpha = std::max(alpha, stand_pat);
        best_score = stand_pat;
    }

    if (!checked && legal_moves.empty()) {
        MoveList all_moves;
        generate_pseudo_legal(position, all_moves, GenType::All);
        if (!has_any_legal_move(position, all_moves)) return 0;
    }

    // Build tactical move buffer (fixed capacity), with TT move first
    std::array<std::pair<int, Move>, MoveList::capacity> q_buffer;
    int q_count = 0;

    bool tt_used = false;
    const bool tactical_tt_move = tt_move.has_flag(MoveFlag::Capture) ||
        tt_move.has_flag(MoveFlag::EnPassant) || tt_move.has_flag(MoveFlag::Promotion);
    if (tt_move.is_valid() && (checked || tactical_tt_move)) {
        StateInfo tt_state;
        if (position.make_move(tt_move, tt_state)) {
            if (king_is_safe_after_move(position, opposite(position.side_to_move()))) {
                position.unmake_move(tt_move, tt_state);
                tt_used = true;
                q_buffer[static_cast<std::size_t>(q_count++)] = {2'000'000, tt_move};
                if (tt_move.has_flag(MoveFlag::Promotion) ||
                    tt_move.has_flag(MoveFlag::EnPassant)) {
                    ++context.picker_stats.promotions_ep_exempted;
                }
            } else {
                position.unmake_move(tt_move, tt_state);
            }
        }
    }

    bool pruned_this_node = false;
    for (std::size_t i = 0; i < legal_moves.size(); ++i) {
        const Move m = legal_moves[i];
        if (!m.is_valid()) continue;
        if (tt_used && m == tt_move) continue;
        if (!checked && !m.has_flag(MoveFlag::Capture) &&
            !m.has_flag(MoveFlag::EnPassant) && !m.has_flag(MoveFlag::Promotion)) {
            continue;
        }

        // SEE pruning: only in non-check nodes, for ordinary captures only.
        // Never prune the TT move, promotions, en-passant, or checking captures.
        if (!checked && m.has_flag(MoveFlag::Capture) &&
            !m.has_flag(MoveFlag::Promotion) && !m.has_flag(MoveFlag::EnPassant)) {
            const Piece victim = position.piece_on(m.to());
            bool checking_capture = false;
            const auto is_checking_capture = [&] {
                const Color moving_side = position.side_to_move();
                StateInfo state;
                if (!position.make_move(m, state)) return false;
                const bool result = king_is_safe_after_move(position, moving_side) &&
                    in_check(position);
                position.unmake_move(m, state);
                return result;
            };
            if (stand_pat + victim_value(victim) + 120 < alpha) {
                checking_capture = is_checking_capture();
                if (!checking_capture) {
                    ++context.picker_stats.captures_pruned_by_see;
                    pruned_this_node = true;
                    continue;
                }
                ++context.picker_stats.checking_captures_exempted;
            }
            ++context.picker_stats.see_pruning_calls;
            if (!see_ge(position, m, 0)) {
                if (!checking_capture) checking_capture = is_checking_capture();
                if (!checking_capture) {
                    ++context.picker_stats.captures_pruned_by_see;
                    pruned_this_node = true;
                    continue;
                }
                if (stand_pat + victim_value(victim) + 120 >= alpha) {
                    ++context.picker_stats.checking_captures_exempted;
                }
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
            score += capture_history_[static_cast<std::size_t>(type_of(attacker))]
                [static_cast<std::size_t>(type_of(victim))][square_index(m.to())] / 2;
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
    int legal_count = 0;
    for (int i = 0; i < q_count; ++i) {
        int best_index = i;
        for (int j = i + 1; j < q_count; ++j) {
            if (q_buffer[static_cast<std::size_t>(j)].first >
                q_buffer[static_cast<std::size_t>(best_index)].first) {
                best_index = j;
            }
        }
        std::swap(q_buffer[static_cast<std::size_t>(i)],
                  q_buffer[static_cast<std::size_t>(best_index)]);
        const Move move = q_buffer[static_cast<std::size_t>(i)].second;
        StateInfo state;
        if (!position.make_move(move, state, context.nnue != nullptr)) {
            continue;
        }
        if (!king_is_safe_after_move(position, opposite(position.side_to_move()))) {
            position.unmake_move(move, state);
            continue;
        }
        if (context.nnue) context.nnue->push(position, state);
        ++legal_count;
        context.keys.push_back(position.key());
        table_.prefetch(position.key());
        auto& child = context.stack[static_cast<std::size_t>(ply + 1)];
        child = SearchStackEntry{};
        child.current_move = move;
        child.extension_count = context.stack[static_cast<std::size_t>(ply)].extension_count;
        PvLine child_pv;
        const int score = -quiescence(position, -beta, -alpha, ply + 1, context, child_pv);
        context.keys.pop_back();
        if (context.nnue) context.nnue->pop();
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
                             position.rule50(), raw_static_eval, false);
                return best_score;
            }
        }
    }

    if (checked && legal_count == 0) {
        return -search_mate_score + ply;
    }

    // Store qsearch result in TT
    if (legal_count > 0 || !checked) {
        Bound bound = Bound::Upper;
        if (best_score <= original_alpha) {
            bound = Bound::Upper;
        } else if (best_score >= beta) {
            bound = Bound::Lower;
        } else if (!pruned_this_node && (pv_node || checked) && best_move.is_valid()) {
            bound = Bound::Exact;
        }
        table_.store(position.key(), best_move, best_score, 0, bound, ply,
                     position.rule50(), raw_static_eval, pv_node);
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
        // A root task owns a short-lived Context. Reserving a chunk here loses
        // its unused tail when that task returns, which makes the parallel
        // search stop far below its requested node limit. Claim each node from
        // the one shared counter instead: a successful claim is an actual
        // searched node, so there is no reservation to reclaim or overshoot.
        std::uint64_t remaining =
            context.limits.shared_node_budget->load(std::memory_order_relaxed);
        while (remaining != 0) {
            if (context.limits.shared_node_budget->compare_exchange_weak(
                    remaining, remaining - 1, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                ++context.nodes;
                return true;
            }
        }
        context.stopped = true;
        return false;
    }
    ++context.nodes;
    return true;
}

int Searcher::evaluate_position(const Position& position, Context& context) const {
    constexpr std::size_t set_count = 2048;
    const std::uint64_t key = position.key();
    const std::size_t set = static_cast<std::size_t>(key) & (set_count - 1);
    EvalCacheEntry& first = eval_cache_[set * 2];
    EvalCacheEntry& second = eval_cache_[set * 2 + 1];
    if (first.valid && first.key == key) return first.score;
    if (second.valid && second.key == key) return second.score;
    const int score = context.nnue ? context.nnue->evaluate(position) : evaluate(position);
    EvalCacheEntry& replacement = !first.valid ? first : !second.valid ? second :
        (((key >> 17) & 1U) != 0U ? first : second);
    replacement = EvalCacheEntry{key, score, true};
    return score;
}

int Searcher::maximum_ply_score(Position& position, int ply, Context& context) const {
    if (!in_check(position)) {
        return evaluate_position(position, context);
    }
    MoveList evasions;
    generate_legal(position, evasions);
    return evasions.empty() ? -search_mate_score + ply : 0;
}

bool Searcher::is_repetition(const Context& context, std::uint64_t key) {
    if (context.keys.empty()) return false;
    int occurrences = 0;
    std::size_t index = context.keys.size() - 1;
    while (true) {
        if (context.keys[index] == key && ++occurrences >= 3) return true;
        if (index < 2) break;
        index -= 2;
    }
    return false;
}

}  // namespace blaze
