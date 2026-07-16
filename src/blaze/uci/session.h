#ifndef BLAZE_UCI_SESSION_H
#define BLAZE_UCI_SESSION_H

#include "blaze/core/position.h"
#include "blaze/search/transposition_table.h"
#include "blaze/uci/limits.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <istream>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace blaze {

class UciSession {
public:
    explicit UciSession(std::ostream& output);
    ~UciSession();

    UciSession(const UciSession&) = delete;
    UciSession& operator=(const UciSession&) = delete;

    void run(std::istream& input);
    [[nodiscard]] bool process_line(std::string_view line);
    [[nodiscard]] std::string current_fen() const { return position_.to_fen(); }

private:
    std::ostream& output_;
    mutable std::mutex output_mutex_;
    TranspositionTable table_{16};
    Position position_;
    std::vector<std::uint64_t> history_;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> suppress_result_{false};
    int threads_ = 1;
    std::chrono::milliseconds move_overhead_{30};
    bool use_nnue_ = false;
    std::string eval_file_;
    std::optional<NetworkEvaluator> network_evaluator_;
    bool pondering_ = false;
    std::string ponder_arguments_;

    void write_line(const std::string& line);
    void stop_search();
    [[nodiscard]] bool set_position(std::string_view arguments);
    [[nodiscard]] bool set_option(std::string_view arguments);
    [[nodiscard]] bool start_search(std::string_view arguments);
    [[nodiscard]] bool handle_ponderhit();
    void reset_position();
};

}  // namespace blaze

#endif  // BLAZE_UCI_SESSION_H
