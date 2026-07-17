#include "blaze/search/selectivity.h"
#include "test_support.h"

namespace blaze {
namespace {

TEST_CASE(adaptive_lmr_rewards_good_moves_and_penalizes_late_bad_moves) {
    const int good = late_move_reduction(
        SelectivityFeatures{.depth = 8, .move_count = 8, .pv_node = false,
                            .improving = true, .history = 8'000,
                            .gives_check = false, .expected_cutoff = false});
    const int bad = late_move_reduction(
        SelectivityFeatures{.depth = 8, .move_count = 8, .pv_node = false,
                            .improving = false, .history = -8'000,
                            .gives_check = false, .expected_cutoff = true});
    CHECK(good >= 0);
    CHECK(bad > good);
    CHECK(bad <= 6);
}

TEST_CASE(adaptive_lmr_keeps_pv_checks_at_full_depth) {
    CHECK_EQ(late_move_reduction(
                 SelectivityFeatures{.depth = 10, .move_count = 12, .pv_node = true,
                                     .improving = false, .history = -10'000,
                                     .gives_check = false, .expected_cutoff = true}),
             0);
    CHECK_EQ(late_move_reduction(
                 SelectivityFeatures{.depth = 10, .move_count = 12, .pv_node = false,
                                     .improving = false, .history = -10'000,
                                     .gives_check = true, .expected_cutoff = true}),
             0);
}

TEST_CASE(dynamic_null_reduction_grows_only_for_a_large_margin) {
    const int close = null_move_reduction(8, 20, 0);
    const int winning = null_move_reduction(8, 500, 0);
    CHECK(winning > close);
    CHECK(close >= 2);
    CHECK(winning <= 6);
}

TEST_CASE(selective_pruning_never_applies_to_pv_or_tactical_nodes) {
    CHECK(!should_razor(3, -500, 0, true, false));
    CHECK(!should_reverse_futility(5, 500, 0, true, false));
    CHECK(!should_late_move_prune(4, 20, -10'000, false, true));
    CHECK(should_late_move_prune(4, 20, -10'000, false, false));
}

TEST_CASE(futility_pruning_requires_a_weak_late_quiet_move) {
    CHECK(!should_futility_prune(4, -100, 0, -10'000, true));
    CHECK(!should_futility_prune(4, 800, 0, 8'000, false));
    CHECK(should_futility_prune(3, -400, 0, -8'000, false));
}

TEST_CASE(quiescence_delta_margin_increases_with_tactical_depth) {
    CHECK(quiescence_delta_margin(0) < quiescence_delta_margin(4));
}

TEST_CASE(singular_extension_only_probes_a_deep_non_pv_tt_candidate) {
    CHECK(!should_try_singular_extension(6, false, 6, 100, 0));
    CHECK(!should_try_singular_extension(8, true, 8, 100, 0));
    CHECK(should_try_singular_extension(8, false, 6, 100, 0));
}

}  // namespace
}  // namespace blaze
