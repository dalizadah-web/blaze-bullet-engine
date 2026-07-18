#ifndef BLAZE_SEARCH_SELECTIVITY_H
#define BLAZE_SEARCH_SELECTIVITY_H

#include <algorithm>
#include <array>

namespace blaze {

inline constexpr bool enable_correction_history = true;
inline constexpr bool enable_razoring_and_reverse_futility = true;

using MoveHistory = std::array<std::array<int, 64>, 64>;
using ContinuationSlice = std::array<int, 64>;
using ContinuationHistory = std::array<ContinuationSlice, 64>;

struct SelectivityFeatures {
    int depth = 0;
    int move_count = 0;
    bool pv_node = false;
    bool improving = false;
    int history = 0;
    bool gives_check = false;
    bool expected_cutoff = false;
};

[[nodiscard]] inline int bounded_extension(int requested, int extensions_on_line) {
    constexpr int maximum_extensions_per_line = 12;
    return std::clamp(
        requested, 0, std::max(0, maximum_extensions_per_line - extensions_on_line));
}

[[nodiscard]] inline int late_move_reduction(const SelectivityFeatures& features) {
    if (features.pv_node || features.gives_check || features.depth < 3 ||
        features.move_count < 3) {
        return 0;
    }
    int reduction = 1;
    if (features.depth >= 6 && features.move_count >= 6) ++reduction;
    if (features.depth >= 10) ++reduction;
    if (features.move_count >= 12) ++reduction;
    if (features.expected_cutoff && features.depth >= 5) ++reduction;
    if (features.improving) --reduction;
    if (features.history >= 4'000) --reduction;
    if (features.history <= -4'000) ++reduction;
    return std::clamp(reduction, 0, std::max(1, features.depth / 2));
}

[[nodiscard]] inline int null_move_reduction(int depth, int static_evaluation, int beta) {
    const int margin = static_evaluation - beta;
    int reduction = 2 + depth / 5;
    if (margin >= 160) ++reduction;
    if (margin >= 400) ++reduction;
    return std::clamp(reduction, 2, std::max(2, depth - 2));
}

[[nodiscard]] inline bool should_razor(
    int depth, int static_evaluation, int alpha, bool pv_node, bool checked) {
    return enable_razoring_and_reverse_futility && !pv_node && !checked && depth <= 3 &&
        static_evaluation + 120 * depth <= alpha;
}

[[nodiscard]] inline bool should_reverse_futility(
    int depth, int static_evaluation, int beta, bool pv_node, bool checked) {
    return enable_razoring_and_reverse_futility && !pv_node && !checked && depth <= 6 &&
        static_evaluation - 110 * depth >= beta;
}

[[nodiscard]] inline bool should_late_move_prune(
    int depth, int move_count, int history, bool gives_check, bool tactical) {
    if (gives_check || tactical || depth < 2) return false;
    const int threshold = 4 + depth * depth / 2;
    return move_count >= threshold && history < -2'000;
}

[[nodiscard]] inline bool should_futility_prune(
    int depth, int static_evaluation, int alpha, int history, bool gives_check) {
    return !gives_check && depth <= 4 && history < -2'000 &&
        static_evaluation + 105 * depth <= alpha;
}

[[nodiscard]] inline int quiescence_delta_margin(int ply) {
    return 90 + std::min(ply, 8) * 16;
}

[[nodiscard]] inline int corrected_static_evaluation(int raw_evaluation, int correction) {
    return raw_evaluation + std::clamp(correction, -4'096, 4'096) / 128;
}

[[nodiscard]] inline int update_correction_history(
    int current, int search_error, int depth) {
    const int weight = std::clamp(depth, 1, 16);
    const int bounded_error = std::clamp(search_error, -512, 512);
    const int update = bounded_error * weight / 16;
    return std::clamp(current + update, -4'096, 4'096);
}

[[nodiscard]] inline bool should_try_singular_extension(
    int depth, bool pv_node, int tt_depth, int tt_score, int alpha) {
    return !pv_node && depth >= 7 && tt_depth >= depth - 2 && tt_score > alpha;
}

}  // namespace blaze

#endif  // BLAZE_SEARCH_SELECTIVITY_H
