#ifndef BLAZE_SEARCH_TIME_MANAGER_H
#define BLAZE_SEARCH_TIME_MANAGER_H

#include <chrono>
#include <cstdint>

namespace blaze {

struct ClockState {
    std::chrono::milliseconds remaining{0};
    std::chrono::milliseconds increment{0};
    int moves_to_go = 0;
    int game_ply = 0;
};

struct LatencyBudget {
    std::chrono::milliseconds configured_overhead{0};
    std::chrono::milliseconds network_p99{0};
};

struct SearchTelemetry {
    double complexity = 1.0;
};

enum class SearchRegime {
    Emergency,
    Bullet,
    Standard,
};

struct MoveBudget {
    std::chrono::milliseconds target{0};
    std::chrono::milliseconds hard{0};
    std::chrono::milliseconds submit_reserve{0};
    SearchRegime regime = SearchRegime::Standard;
    int workers = 1;
    bool emergency = false;
};

class BulletTimeManager {
public:
    [[nodiscard]] static MoveBudget allocate(
        const ClockState& clock,
        const LatencyBudget& latency,
        const SearchTelemetry& telemetry);
    [[nodiscard]] static std::uint64_t clock_poll_interval(SearchRegime regime);
};

}  // namespace blaze

#endif  // BLAZE_SEARCH_TIME_MANAGER_H
