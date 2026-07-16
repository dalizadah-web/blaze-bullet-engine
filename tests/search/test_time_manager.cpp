#include "blaze/search/time_manager.h"

#include "test_support.h"

#include <chrono>

namespace blaze {
namespace {

using namespace std::chrono_literals;

TEST_CASE(pure_increment_bullet_always_has_a_finite_deadline) {
    const MoveBudget one_second = BulletTimeManager::allocate(
        ClockState{0ms, 1000ms, 0, 0}, LatencyBudget{25ms, 40ms}, SearchTelemetry{});
    const MoveBudget two_seconds = BulletTimeManager::allocate(
        ClockState{0ms, 2000ms, 0, 0}, LatencyBudget{25ms, 40ms}, SearchTelemetry{});

    CHECK(one_second.target > 0ms);
    CHECK(one_second.hard > one_second.target);
    CHECK(one_second.hard < 1000ms);
    CHECK(two_seconds.target > one_second.target);
    CHECK(two_seconds.hard < 2000ms);
    CHECK_EQ(one_second.regime, SearchRegime::Bullet);
}

TEST_CASE(pure_increment_bullet_banks_most_of_each_increment) {
    const MoveBudget one_second = BulletTimeManager::allocate(
        ClockState{0ms, 1000ms, 0, 0}, LatencyBudget{30ms, 0ms}, SearchTelemetry{});
    const MoveBudget two_seconds = BulletTimeManager::allocate(
        ClockState{0ms, 2000ms, 0, 0}, LatencyBudget{30ms, 0ms}, SearchTelemetry{});

    CHECK(one_second.target <= 400ms);
    CHECK(one_second.hard <= 750ms);
    CHECK(two_seconds.target <= 800ms);
    CHECK(two_seconds.hard <= 1500ms);
}

TEST_CASE(clock_budget_keeps_the_measured_submission_reserve) {
    const MoveBudget budget = BulletTimeManager::allocate(
        ClockState{1000ms, 0ms, 0, 12}, LatencyBudget{25ms, 80ms}, SearchTelemetry{});

    CHECK_EQ(budget.submit_reserve.count(), 80);
    CHECK(budget.hard <= 920ms);
    CHECK(budget.target < budget.hard);
}

TEST_CASE(bullet_worker_count_tracks_the_hard_time_budget) {
    const MoveBudget emergency = BulletTimeManager::allocate(
        ClockState{20ms, 0ms, 0, 0}, LatencyBudget{15ms, 0ms}, SearchTelemetry{});
    const MoveBudget tiny = BulletTimeManager::allocate(
        ClockState{100ms, 0ms, 0, 0}, LatencyBudget{20ms, 0ms}, SearchTelemetry{});
    const MoveBudget normal = BulletTimeManager::allocate(
        ClockState{2000ms, 0ms, 0, 0}, LatencyBudget{25ms, 0ms}, SearchTelemetry{});

    CHECK(emergency.emergency);
    CHECK_EQ(emergency.workers, 1);
    CHECK(tiny.workers <= 2);
    CHECK(normal.workers >= tiny.workers);
}

TEST_CASE(complex_positions_receive_more_target_time_without_breaking_the_hard_limit) {
    SearchTelemetry simple;
    simple.complexity = 0.65;
    SearchTelemetry complex;
    complex.complexity = 1.45;

    const ClockState clock{5000ms, 100ms, 0, 20};
    const LatencyBudget latency{30ms, 20ms};
    const MoveBudget fast = BulletTimeManager::allocate(clock, latency, simple);
    const MoveBudget deep = BulletTimeManager::allocate(clock, latency, complex);

    CHECK(deep.target > fast.target);
    CHECK_EQ(deep.hard, fast.hard);
}

}  // namespace
}  // namespace blaze
