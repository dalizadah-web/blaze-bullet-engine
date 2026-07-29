#ifndef BLAZE_SEARCH_PARAMETERS_H
#define BLAZE_SEARCH_PARAMETERS_H

#include <array>
#include <string>
#include <string_view>

namespace blaze {

struct SearchParameters {
    int rfp_base = 65;
    int rfp_depth = 75;
    int rfp_depth_squared = 18;
    int rfp_improving = 70;
    int rfp_max_depth = 9;

    int razor_base = 180;
    int razor_depth_squared = 150;
    int razor_max_depth = 3;

    int null_min_depth = 3;
    int null_eval_divisor = 180;
    int null_eval_cap = 3;
    int null_base_reduction = 3;
    int null_depth_divisor = 4;
    int null_verify_depth = 10;

    int probcut_min_depth = 3;
    int probcut_base = 120;
    int probcut_depth = 10;
    int probcut_shallow_reduction = 2;
    int probcut_deep_extra = 2;

    int singular_min_depth = 7;
    int singular_beta_base = 18;
    int singular_beta_depth = 2;
    int singular_double_margin = 105;
    int singular_double_depth = 10;

    int quiet_pruning_max_depth = 10;
    int tactical_pruning_max_depth = 8;
    int lmp_nonimproving_base = 3;
    int lmp_improving_extra = 2;
    int lmp_depth_cap = 10;
    int lmp_improving_divisor = 2;
    int lmp_nonimproving_divisor_extra = 1;

    int futility_max_depth = 7;
    int futility_base = 80;
    int futility_depth = 95;
    int futility_depth_squared = 35;
    int negative_history_max_depth = 6;
    int negative_history_per_depth = 3500;

    int selective_check_min_depth = 3;
    int selective_check_move_count = 3;
    int recapture_max_depth = 10;

    int lmr_offset_hundredths = 65;
    int lmr_divisor_hundredths = 230;
    int lmr_high_history = 6000;
    int lmr_low_history_magnitude = 4000;
    int lmr_research_margin = 12;

    int history_success_multiplier = 32;
    int history_penalty_multiplier = 16;
    int qsearch_delta_margin = 120;
};

struct SearchParameterDescriptor {
    std::string_view name;
    int SearchParameters::*field;
    int minimum;
    int maximum;
};

inline constexpr auto search_parameter_descriptors = std::to_array<SearchParameterDescriptor>({
    {"rfp_base", &SearchParameters::rfp_base, 20, 160},
    {"rfp_depth", &SearchParameters::rfp_depth, 30, 160},
    {"rfp_depth_squared", &SearchParameters::rfp_depth_squared, 0, 50},
    {"rfp_improving", &SearchParameters::rfp_improving, 0, 100},
    {"rfp_max_depth", &SearchParameters::rfp_max_depth, 4, 12},
    {"razor_base", &SearchParameters::razor_base, 60, 360},
    {"razor_depth_squared", &SearchParameters::razor_depth_squared, 40, 320},
    {"razor_max_depth", &SearchParameters::razor_max_depth, 1, 5},
    {"null_min_depth", &SearchParameters::null_min_depth, 2, 6},
    {"null_eval_divisor", &SearchParameters::null_eval_divisor, 60, 400},
    {"null_eval_cap", &SearchParameters::null_eval_cap, 0, 6},
    {"null_base_reduction", &SearchParameters::null_base_reduction, 1, 5},
    {"null_depth_divisor", &SearchParameters::null_depth_divisor, 2, 8},
    {"null_verify_depth", &SearchParameters::null_verify_depth, 6, 16},
    {"probcut_min_depth", &SearchParameters::probcut_min_depth, 3, 6},
    {"probcut_base", &SearchParameters::probcut_base, 20, 300},
    {"probcut_depth", &SearchParameters::probcut_depth, 0, 40},
    {"probcut_shallow_reduction", &SearchParameters::probcut_shallow_reduction, 1, 3},
    {"probcut_deep_extra", &SearchParameters::probcut_deep_extra, 1, 4},
    {"singular_min_depth", &SearchParameters::singular_min_depth, 5, 12},
    {"singular_beta_base", &SearchParameters::singular_beta_base, 0, 80},
    {"singular_beta_depth", &SearchParameters::singular_beta_depth, 0, 10},
    {"singular_double_margin", &SearchParameters::singular_double_margin, 20, 300},
    {"singular_double_depth", &SearchParameters::singular_double_depth, 7, 16},
    {"quiet_pruning_max_depth", &SearchParameters::quiet_pruning_max_depth, 4, 14},
    {"tactical_pruning_max_depth", &SearchParameters::tactical_pruning_max_depth, 3, 12},
    {"lmp_nonimproving_base", &SearchParameters::lmp_nonimproving_base, 1, 8},
    {"lmp_improving_extra", &SearchParameters::lmp_improving_extra, 0, 6},
    {"lmp_depth_cap", &SearchParameters::lmp_depth_cap, 4, 16},
    {"lmp_improving_divisor", &SearchParameters::lmp_improving_divisor, 1, 6},
    {"lmp_nonimproving_divisor_extra", &SearchParameters::lmp_nonimproving_divisor_extra, 0, 5},
    {"futility_max_depth", &SearchParameters::futility_max_depth, 3, 10},
    {"futility_base", &SearchParameters::futility_base, 0, 300},
    {"futility_depth", &SearchParameters::futility_depth, 20, 200},
    {"futility_depth_squared", &SearchParameters::futility_depth_squared, 0, 90},
    {"negative_history_max_depth", &SearchParameters::negative_history_max_depth, 3, 10},
    {"negative_history_per_depth", &SearchParameters::negative_history_per_depth, 500, 8000},
    {"selective_check_min_depth", &SearchParameters::selective_check_min_depth, 1, 6},
    {"selective_check_move_count", &SearchParameters::selective_check_move_count, 1, 8},
    {"recapture_max_depth", &SearchParameters::recapture_max_depth, 4, 16},
    {"lmr_offset_hundredths", &SearchParameters::lmr_offset_hundredths, 0, 150},
    {"lmr_divisor_hundredths", &SearchParameters::lmr_divisor_hundredths, 100, 400},
    {"lmr_high_history", &SearchParameters::lmr_high_history, 1000, 16000},
    {"lmr_low_history_magnitude", &SearchParameters::lmr_low_history_magnitude, 500, 16000},
    {"lmr_research_margin", &SearchParameters::lmr_research_margin, 0, 64},
    {"history_success_multiplier", &SearchParameters::history_success_multiplier, 8, 96},
    {"history_penalty_multiplier", &SearchParameters::history_penalty_multiplier, 4, 64},
    {"qsearch_delta_margin", &SearchParameters::qsearch_delta_margin, 0, 400},
});

[[nodiscard]] bool parse_search_parameters(
    std::string_view payload, SearchParameters& output, std::string& error);
[[nodiscard]] std::string serialize_search_parameters(const SearchParameters& parameters);

}  // namespace blaze

#endif  // BLAZE_SEARCH_PARAMETERS_H
