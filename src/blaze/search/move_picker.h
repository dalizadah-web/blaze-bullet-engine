#ifndef BLAZE_SEARCH_MOVE_PICKER_H
#define BLAZE_SEARCH_MOVE_PICKER_H

#include "blaze/core/move.h"
#include "blaze/core/movegen.h"
#include "blaze/core/types.h"
#include "blaze/search/see.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace blaze {

class MovePicker {
public:
    static constexpr int MAX_MOVES = 256;

    enum Stage : std::uint8_t {
        TT_MOVE,
        GOOD_CAPTURES,
        STRONG_QUIETS,
        BAD_CAPTURES,
        REMAINING_QUIETS,
        DONE
    };

    struct Stats {
        std::uint64_t first_move_cutoffs = 0;
        std::uint64_t cutoff_move_sum = 0;
        std::uint64_t cutoff_count = 0;
        std::uint64_t see_calls = 0;
        std::chrono::nanoseconds see_time{0};
        std::uint64_t moves_scored = 0;
        std::uint64_t moves_searched = 0;
        std::array<std::uint64_t, 5> stage_counts{};
        std::array<std::uint64_t, 5> stage_cutoffs{};
        std::uint64_t qnodes = 0;
        std::uint64_t tactical_moves_generated = 0;
        std::uint64_t see_pruning_calls = 0;
        std::uint64_t captures_pruned_by_see = 0;
        std::uint64_t checking_captures_exempted = 0;
        std::uint64_t promotions_ep_exempted = 0;
        std::uint64_t qnodes_with_see_pruning = 0;

        void accumulate(const Stats& other) {
            first_move_cutoffs += other.first_move_cutoffs;
            cutoff_move_sum += other.cutoff_move_sum;
            cutoff_count += other.cutoff_count;
            see_calls += other.see_calls;
            see_time += other.see_time;
            moves_scored += other.moves_scored;
            moves_searched += other.moves_searched;
            for (int i = 0; i < 5; ++i) {
                stage_counts[i] += other.stage_counts[i];
                stage_cutoffs[i] += other.stage_cutoffs[i];
            }
            qnodes += other.qnodes;
            tactical_moves_generated += other.tactical_moves_generated;
            see_pruning_calls += other.see_pruning_calls;
            captures_pruned_by_see += other.captures_pruned_by_see;
            checking_captures_exempted += other.checking_captures_exempted;
            promotions_ep_exempted += other.promotions_ep_exempted;
            qnodes_with_see_pruning += other.qnodes_with_see_pruning;
        }
    };

    MovePicker(const Position& pos,
               const MoveList& moves,
               Move tt_move,
               const Move killers[2],
               Move counter_move,
               Color us,
               Move previous_move,
               const std::array<std::array<int, 64>, 64>& history,
               const std::array<std::array<std::array<int, 64>, 7>, 7>& capture_history,
               const std::array<std::array<int, 64>, 64>& pawn_history,
               const std::array<std::array<int, 64>, 64>& continuation_history,
               const std::array<std::array<int, 64>, 64>& low_ply_history,
               const std::array<std::array<int, 64>, 64>& threat_history,
               const std::array<std::array<int, 64>, 7>& per_piece_history,
               int ply)
        : pos_(pos), tt_move_(tt_move), counter_move_(counter_move),
          previous_move_(previous_move), us_(us), history_(history), capture_history_(capture_history),
          pawn_history_(pawn_history), continuation_history_(continuation_history),
          low_ply_history_(low_ply_history), threat_history_(threat_history),
          per_piece_history_(per_piece_history), ply_(ply) {
        killer0_ = killers[0];
        killer1_ = killers[1];
        const std::size_t n = moves.size();
        count_ = n < MAX_MOVES ? static_cast<int>(n) : MAX_MOVES;
        for (int i = 0; i < count_; ++i) {
            moves_[i] = moves[static_cast<std::size_t>(i)];
        }
    }

    Move next() {
        while (true) {
            switch (stage_) {
            case TT_MOVE: {
                stage_ = GOOD_CAPTURES;
                if (tt_move_.is_valid()) {
                    for (int i = 0; i < count_; ++i) {
                        if (moves_[i] == tt_move_) {
                            selected_[i] = true;
                            break;
                        }
                    }
                    return tt_move_;
                }
                break;
            }
            case GOOD_CAPTURES: {
                if (buf_pos_ == 0) {
                    fill_good_captures();
                }
                if (buf_pos_ < buf_count_) {
                    ++moves_searched_;
                    ++stage_counts_[0];
                    return pop_best_buffer();
                }
                stage_ = STRONG_QUIETS;
                buf_pos_ = 0;
                buf_count_ = 0;
                break;
            }
            case STRONG_QUIETS: {
                if (buf_pos_ == 0) {
                    fill_strong_quiets();
                }
                if (buf_pos_ < buf_count_) {
                    ++moves_searched_;
                    ++stage_counts_[1];
                    return pop_best_buffer();
                }
                stage_ = BAD_CAPTURES;
                buf_pos_ = 0;
                buf_count_ = 0;
                break;
            }
            case BAD_CAPTURES: {
                if (buf_pos_ == 0) {
                    fill_bad_captures();
                }
                if (buf_pos_ < buf_count_) {
                    ++moves_searched_;
                    ++stage_counts_[2];
                    return pop_best_buffer();
                }
                stage_ = REMAINING_QUIETS;
                buf_pos_ = 0;
                buf_count_ = 0;
                break;
            }
            case REMAINING_QUIETS: {
                if (buf_pos_ == 0) {
                    fill_remaining_quiets();
                }
                if (buf_pos_ < buf_count_) {
                    ++moves_searched_;
                    ++stage_counts_[3];
                    return pop_best_buffer();
                }
                stage_ = DONE;
                break;
            }
            case DONE:
                return Move{};
            }
        }
    }

    void on_cutoff(int move_index) {
        ++cutoff_count_;
        cutoff_move_sum_ += static_cast<std::uint64_t>(move_index);
        if (move_index == 1) {
            ++first_move_cutoffs_;
        }
        if (stage_ >= TT_MOVE && stage_ < DONE) {
            ++stage_cutoffs_[static_cast<int>(stage_) - 1];
        }
    }

    void reset_for_next_node() {
        stage_ = TT_MOVE;
        buf_pos_ = 0;
        buf_count_ = 0;
        for (int i = 0; i < count_; ++i) {
            selected_[i] = false;
            capture_known_[i] = false;
            capture_good_[i] = false;
        }
    }

    Stats collect_stats() const {
        Stats s;
        s.first_move_cutoffs = first_move_cutoffs_;
        s.cutoff_move_sum = cutoff_move_sum_;
        s.cutoff_count = cutoff_count_;
        s.see_calls = see_calls_;
        s.see_time = see_time_;
        s.moves_scored = moves_scored_;
        s.moves_searched = moves_searched_;
        s.stage_counts = stage_counts_;
        s.stage_cutoffs = stage_cutoffs_;
        return s;
    }

    void accumulate_stats(const Stats& s) {
        first_move_cutoffs_ += s.first_move_cutoffs;
        cutoff_move_sum_ += s.cutoff_move_sum;
        cutoff_count_ += s.cutoff_count;
        see_calls_ += s.see_calls;
        see_time_ += s.see_time;
        moves_scored_ += s.moves_scored;
        moves_searched_ += s.moves_searched;
        for (int i = 0; i < 5; ++i) {
            stage_counts_[i] += s.stage_counts[i];
            stage_cutoffs_[i] += s.stage_cutoffs[i];
        }
    }

private:
    const Position& pos_;
    Move tt_move_;
    Move counter_move_;
    Move previous_move_;
    Move killer0_, killer1_;
    Color us_;
    const std::array<std::array<int, 64>, 64>& history_;
    const std::array<std::array<std::array<int, 64>, 7>, 7>& capture_history_;
    const std::array<std::array<int, 64>, 64>& pawn_history_;
    const std::array<std::array<int, 64>, 64>& continuation_history_;
    const std::array<std::array<int, 64>, 64>& low_ply_history_;
    const std::array<std::array<int, 64>, 64>& threat_history_;
    const std::array<std::array<int, 64>, 7>& per_piece_history_;
    int ply_ = 0;

    Move moves_[MAX_MOVES];
    bool selected_[MAX_MOVES] = {false};
    int count_ = 0;

    struct ScoredMove {
        Move move;
        int score;
    };
    ScoredMove buffer_[MAX_MOVES];
    int buf_count_ = 0;
    int buf_pos_ = 0;
    Stage stage_ = TT_MOVE;
    bool capture_known_[MAX_MOVES] = {false};
    bool capture_good_[MAX_MOVES] = {false};

    std::uint64_t first_move_cutoffs_ = 0;
    std::uint64_t cutoff_move_sum_ = 0;
    std::uint64_t cutoff_count_ = 0;
    std::uint64_t see_calls_ = 0;
    std::chrono::nanoseconds see_time_{0};
    std::uint64_t moves_scored_ = 0;
    std::uint64_t moves_searched_ = 0;
    std::array<std::uint64_t, 5> stage_counts_{};
    std::array<std::uint64_t, 5> stage_cutoffs_{};

    static int victim_value(Piece p) {
        constexpr std::array<int, 7> values = {0, 100, 320, 335, 500, 900, 20000};
        return values[static_cast<std::size_t>(type_of(p))];
    }

    int score_good_capture(Move m) {
        ++moves_scored_;
        if (m.has_flag(MoveFlag::Promotion)) {
            return 800'000 + victim_value(make_piece(us_, m.promotion()));
        }
        const Piece victim = m.has_flag(MoveFlag::EnPassant)
            ? make_piece(opposite(us_), PieceType::Pawn)
            : pos_.piece_on(m.to());
        const Piece attacker = pos_.piece_on(m.from());
        int score = victim_value(victim) * 16 - victim_value(attacker);
        const int ti = square_index(m.to());
        score += capture_history_[static_cast<std::size_t>(type_of(attacker))]
            [static_cast<std::size_t>(type_of(victim))][static_cast<std::size_t>(ti)];
        return score;
    }

    int score_bad_capture(Move m) {
        ++moves_scored_;
        const Piece victim = m.has_flag(MoveFlag::EnPassant)
            ? make_piece(opposite(us_), PieceType::Pawn)
            : pos_.piece_on(m.to());
        const Piece attacker = pos_.piece_on(m.from());
        return victim_value(victim) * 16 - victim_value(attacker);
    }

    int score_quiet(Move m) {
        ++moves_scored_;
        const int fi = square_index(m.from());
        const int ti = square_index(m.to());
        int score = 0;
        score += history_[fi][ti];
        if (previous_move_.is_valid()) {
            score += continuation_history_[square_index(previous_move_.to())][ti];
            score += threat_history_[square_index(previous_move_.to())][ti];
        }
        const Piece attacker = pos_.piece_on(m.from());
        score += per_piece_history_[static_cast<std::size_t>(type_of(attacker))][ti];
        if (m == killer0_ || m == killer1_ || m == counter_move_) {
            score += 2'000'000;
        }
        if (type_of(pos_.piece_on(static_cast<Square>(fi))) == PieceType::Pawn) {
            score += pawn_history_[fi][ti];
        }
        if (ply_ < 4) {
            score += low_ply_history_[fi][ti];
        }
        return score;
    }

    bool is_quiet(Move m) const {
        return !m.has_flag(MoveFlag::Capture) &&
               !m.has_flag(MoveFlag::EnPassant) &&
               !m.has_flag(MoveFlag::Promotion);
    }

    Move pop_best_buffer() {
        int best = buf_pos_;
        for (int i = buf_pos_ + 1; i < buf_count_; ++i) {
            if (buffer_[i].score > buffer_[best].score) {
                best = i;
            }
        }
        std::swap(buffer_[buf_pos_], buffer_[best]);
        return buffer_[buf_pos_++].move;
    }

    bool classify_capture(int index) {
        if (capture_known_[index]) {
            return capture_good_[index];
        }
        const Move move = moves_[index];
        capture_known_[index] = true;
        if (move.has_flag(MoveFlag::Promotion)) {
            capture_good_[index] = true;
            return true;
        }
        capture_good_[index] = see_ge(pos_, move, 0);
        ++see_calls_;
        return capture_good_[index];
    }

    void fill_good_captures() {
        buf_count_ = 0;
        const auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < count_; ++i) {
            if (selected_[i]) continue;
            const Move m = moves_[i];
            if (m == tt_move_) continue;
            if (m.has_flag(MoveFlag::Capture) || m.has_flag(MoveFlag::EnPassant) ||
                m.has_flag(MoveFlag::Promotion)) {
                bool good = false;
                if (m.has_flag(MoveFlag::Promotion)) {
                    good = true;
                } else {
                    good = classify_capture(i);
                }
                if (good) {
                    selected_[i] = true;
                    buffer_[buf_count_++] = {m, score_good_capture(m)};
                }
            }
        }
        const auto end = std::chrono::high_resolution_clock::now();
        see_time_ += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    }

    void fill_strong_quiets() {
        buf_count_ = 0;
        for (int i = 0; i < count_; ++i) {
            if (selected_[i]) continue;
            const Move m = moves_[i];
            if (m == tt_move_) continue;
            if (is_quiet(m)) {
                selected_[i] = true;
                buffer_[buf_count_++] = {m, score_quiet(m)};
            }
        }
    }

    void fill_bad_captures() {
        buf_count_ = 0;
        for (int i = 0; i < count_; ++i) {
            if (selected_[i]) continue;
            const Move m = moves_[i];
            if (m == tt_move_) continue;
            if (m.has_flag(MoveFlag::Capture) || m.has_flag(MoveFlag::EnPassant)) {
                if (!classify_capture(i)) {
                    selected_[i] = true;
                    buffer_[buf_count_++] = {m, score_bad_capture(m)};
                }
            }
        }
    }

    void fill_remaining_quiets() {
        buf_count_ = 0;
        for (int i = 0; i < count_; ++i) {
            if (selected_[i]) continue;
            const Move m = moves_[i];
            if (m == tt_move_) continue;
            selected_[i] = true;
            buffer_[buf_count_++] = {m, is_quiet(m) ? score_quiet(m) : score_good_capture(m)};
        }
    }
};

}  // namespace blaze

#endif
