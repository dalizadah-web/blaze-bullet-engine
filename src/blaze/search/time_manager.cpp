#include "blaze/search/time_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace blaze {
namespace {

using Milliseconds = std::chrono::milliseconds;

Milliseconds scaled(Milliseconds duration, double factor) {
    return Milliseconds(static_cast<std::int64_t>(
        std::llround(static_cast<double>(duration.count()) * factor)));
}

int default_moves_left(int game_ply) {
    if (game_ply < 20) return 16;
    if (game_ply < 60) return 12;
    return 9;
}

int recommended_workers(Milliseconds hard) {
    if (hard < Milliseconds(8)) return 1;
    if (hard < Milliseconds(25)) return 2;
    if (hard < Milliseconds(80)) return 4;
    return 8;
}

}  // namespace

MoveBudget BulletTimeManager::allocate(
    const ClockState& clock,
    const LatencyBudget& latency,
    const SearchTelemetry& telemetry) {
    MoveBudget result;
    result.submit_reserve = std::max(latency.configured_overhead, latency.network_p99);

    // Pure-increment controls report zero main time. Treat the next increment as
    // the bankroll for this move, while still reserving submission latency.
    const Milliseconds bankroll = clock.remaining > Milliseconds(0)
        ? clock.remaining
        : clock.increment;
    if (bankroll <= Milliseconds(0)) {
        result.regime = SearchRegime::Emergency;
        result.emergency = true;
        return result;
    }

    const Milliseconds usable = std::max(
        bankroll - result.submit_reserve, Milliseconds(1));
    const int moves = clock.moves_to_go > 0
        ? clock.moves_to_go
        : default_moves_left(clock.game_ply);

    // Pure-increment bullet must bank clock before buying deeper searches. With
    // an existing bankroll we can spend a little more of each increment.
    const double increment_fraction = clock.remaining > Milliseconds(0) ? 0.45 : 0.30;
    Milliseconds base = usable / moves + scaled(clock.increment, increment_fraction);
    base = std::max(base, Milliseconds(1));

    // The hard limit is deliberately independent of position complexity. A
    // volatile position may use more of the allowance, but never risks the clock.
    result.hard = std::min(
        usable, std::max(scaled(base, 1.65), base + Milliseconds(1)));
    if (clock.increment > Milliseconds(0) && bankroll <= Milliseconds(10000)) {
        const Milliseconds growth_cap =
            scaled(clock.increment, 0.60) + scaled(clock.remaining, 0.08);
        result.hard = std::min(
            result.hard, std::max(growth_cap, base + Milliseconds(1)));
    }
    if (result.hard > Milliseconds(1)) {
        const double complexity = std::clamp(telemetry.complexity, 0.65, 1.45);
        result.target = std::clamp(
            scaled(base, complexity), Milliseconds(1), result.hard - Milliseconds(1));
    } else {
        result.target = result.hard;
    }

    result.emergency = result.hard < Milliseconds(8);
    result.regime = result.emergency
        ? SearchRegime::Emergency
        : (bankroll <= Milliseconds(10000) || clock.increment > Milliseconds(0)
            ? SearchRegime::Bullet
            : SearchRegime::Standard);
    result.workers = recommended_workers(result.hard);
    return result;
}

std::uint64_t BulletTimeManager::clock_poll_interval(SearchRegime regime) {
    switch (regime) {
        case SearchRegime::Emergency: return 32;
        case SearchRegime::Bullet: return 128;
        case SearchRegime::Standard: return 1024;
    }
    return 128;
}

}  // namespace blaze
