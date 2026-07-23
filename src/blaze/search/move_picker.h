#ifndef BLAZE_SEARCH_MOVE_PICKER_H
#define BLAZE_SEARCH_MOVE_PICKER_H

#include "blaze/core/movegen.h"
#include "blaze/search/see.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace blaze {

class MovePicker {
public:
    static constexpr int MAX_MOVES = 256;

    // Must match the order we want to search them in.
    // The next() method walks through these stages in declaration order.
    enum Stage : std::uint8_t {
        TT_MOVE,
        GOOD_CAPTURES,
        STRONG_QUIETS,
        BAD_CAPTURES,
        REMAINING_QUIETS,
        DONE
    };

    struct Stats {
        std::uint64_t first_move_cutoffs = 0;  // #nodes where move 0 caused beta cutoff
        std::uint64_t cutoff_move_sum = 0;     // sum of move indices at cutoff
        std::uint64_t cutoff_count = 0;        // #cutoffs for averaging
        std::uint64_t see_calls = 0;
        std::chrono::nanoseconds see_time{0};
        std::uint64_t moves_scored = 0;
        std::uint64_t moves_searched = 0;
        // Per-stage move counts (how many searched from each stage)
        std::array<std::uint64_t, 5> stage_counts{};
        // Per-stage cutoff counts
        std::array<std::uint64_t, 5> stage_cutoffs{};

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
        }
    };

    MovePicker(const Position& pos,
               const MoveList& moves,
               Move tt_move,
               const Move killers[2],
               Move counter_move,
               Color us,
               const std::array<std::array<int, 64>, 64>& history)
        : pos_(pos), tt_move_(tt_move), counter_move_(counter_move),
          us_(us), history_(history) {
        killer0_ = killers[0];
        killer1_ = killers[1];
        const std::size_t n = moves.size();
        count_ = n < MAX_MOVES ? static_cast<int>(n) : MAX_MOVES;
        for (int i = 0; i < count_; ++i) {
            moves_[i] = moves[static_cast<std::size_t>(i)];
        }
    }

    // Returns the next move to search. Returns an invalid Move when exhausted.
    Move next() {
        while (true) {
            switch (stage_) {
            case TT_MOVE: {
                stage_ = GOOD_CAPTURES;
                if (tt_move_.is_valid()) {
                    selected_[0] = true;
                    return tt_move_;
                }
                break;
            }
            case GOOD_CAPTURES: {
                if (buf_pos_ == 0) {
                    fill_good_captures();
                    if (buf_count_ > 0) {
                        partial_sort_buffer();
                    }
                }
                if (buf_pos_ < buf_count_) {
                    ++moves_searched_;
                    ++stage_counts_[0];
                    return buffer_[buf_pos_++].move;
                }
                stage_ = STRONG_QUIETS;
                buf_pos_ = 0;
                buf_count_ = 0;
                break;
            }
            case STRONG_QUIETS: {
                if (buf_pos_ == 0) {
                    fill_strong_quiets();
                    if (buf_count_ > 0) {
                        partial_sort_buffer();
                    }
                }
                if (buf_pos_ < buf_count_) {
                    ++moves_searched_;
                    ++stage_counts_[1];
                    return buffer_[buf_pos_++].move;
                }
                stage_ = BAD_CAPTURES;
                buf_pos_ = 0;
                buf_count_ = 0;
                break;
            }
            case BAD_CAPTURES: {
                if (buf_pos_ == 0) {
                    fill_bad_captures();
                    if (buf_count_ > 0) {
                        partial_sort_buffer();
                    }
                }
                if (buf_pos_ < buf_count_) {
                    ++moves_searched_;
                    ++stage_counts_[2];
                    return buffer_[buf_pos_++].move;
                }
                stage_ = REMAINING_QUIETS;
                buf_pos_ = 0;
                buf_count_ = 0;
                break;
            }
            case REMAINING_QUIETS: {
                if (buf_pos_ == 0) {
                    fill_remaining_quiets();
                    if (buf_count_ > 0) {
                        partial_sort_buffer();
                    }
                }
                if (buf_pos_ < buf_count_) {
                    ++moves_searched_;
                    ++stage_counts_[3];
                    return buffer_[buf_pos_++].move;
                }
                stage_ = DONE;
                break;
            }
            case DONE:
                return Move{};
            }
        }
    }

    // Called after a cutoff (alpha >= beta) at the given move index
    void on_cutoff(int move_index) {
        ++cutoff_count_;
        cutoff_move_sum_ += static_cast<std::uint64_t>(move_index);
        if (move_index == 1) {
            ++first_move_cutoffs_;  // index 1 = second picked move (index 0 is TT)
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
        }
    }

    // Gather stats and return them atomically (for the root node's accumulated view)
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
    Move killer0_, killer1_;
    Color us_;
    const std::array<std::array<int, 64>, 64>& history_;

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

    // Stats accumulators (per-searcher, across all nodes searched)
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
        ++see_calls_;
        // For promotions, score by promotion piece value
        if (m.has_flag(MoveFlag::Promotion)) {
            return 800'000 + victim_value(make_piece(us_, m.promotion()));
        }
        // MVV-LVA for captures
        const Piece victim = m.has_flag(MoveFlag::EnPassant)
            ? make_piece(opposite(us_), PieceType::Pawn)
            : pos_.piece_on(m.to());
        const Piece attacker = pos_.piece_on(m.from());
        return victim_value(victim) * 16 - victim_value(attacker);
    }

    int score_bad_capture(Move m) {
        ++moves_scored_;
        ++see_calls_;
        const Piece victim = m.has_flag(MoveFlag::EnPassant)
            ? make_piece(opposite(us_), PieceType::Pawn)
            : pos_.piece_on(m.to());
        const Piece attacker = pos_.piece_on(m.from());
        return victim_value(victim) * 16 - victim_value(attacker);
    }

    int score_quiet(Move m) {
        ++moves_scored_;
        const std::size_t fi = static_cast<std::size_t>(square_index(m.from()));
        const std::size_t ti = static_cast<std::size_t>(square_index(m.to()));
        const int hist = history_[fi][ti];
        // Add large bonus for killers and countermove
        if (m == killer0_ || m == killer1_ || m == counter_move_) {
            return hist + 2'000'000;
        }
        return hist;
    }

    bool is_quiet(Move m) const {
        return !m.has_flag(MoveFlag::Capture) &&
               !m.has_flag(MoveFlag::EnPassant) &&
               !m.has_flag(MoveFlag::Promotion);
    }

    void partial_sort_buffer() {
        // Bring the highest-scored move to the front so we can iterate.
        // We use a full sort once at entry to the stage; the compiler
        // inlines this nicely for small arrays.
        std::sort(buffer_, buffer_ + buf_count_,
                  [](const ScoredMove& a, const ScoredMove& b) {
                      return a.score > b.score;
                  });
    }

    void fill_good_captures() {
        buf_count_ = 0;
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < count_; ++i) {
            if (selected_[i]) continue;
            const Move m = moves_[i];
            if (m == tt_move_) continue;
            if (m.has_flag(MoveFlag::Capture) || m.has_flag(MoveFlag::EnPassant) ||
                m.has_flag(MoveFlag::Promotion)) {
                // Good capture = SEE >= 0
                bool good = false;
                if (m.has_flag(MoveFlag::Promotion)) {
                    good = true;  // promotions are always good
                } else {
                    good = see_ge(pos_, m, 0);
                }
                if (good) {
                    selected_[i] = true;
                    buffer_[buf_count_++] = {m, score_good_capture(m)};
                }
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
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
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < count_; ++i) {
            if (selected_[i]) continue;
            const Move m = moves_[i];
            if (m == tt_move_) continue;
            if (m.has_flag(MoveFlag::Capture) || m.has_flag(MoveFlag::EnPassant)) {
                if (!see_ge(pos_, m, 0)) {
                    selected_[i] = true;
                    buffer_[buf_count_++] = {m, score_bad_capture(m)};
                }
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        see_time_ += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    }

    void fill_remaining_quiets() {
        buf_count_ = 0;
        for (int i = 0; i < count_; ++i) {
            if (selected_[i]) continue;
            const Move m = moves_[i];
            if (m == tt_move_) continue;
            // Already selected good captures, strong quiets, and bad captures.
            // What's left: captures we didn't classify (unlikely) or
            // anything we missed. Just score remaining non-TT moves.
            selected_[i] = true;
            buffer_[buf_count_++] = {m, is_quiet(m) ? score_quiet(m) : score_good_capture(m)};
        }
    }
};

}  // namespace blaze

#endif
