#ifndef BLAZE_UCI_SESSION_H
#define BLAZE_UCI_SESSION_H

#include "blaze/core/position.h"
#include "blaze/search/transposition_table.h"

#include <atomic>
#include <cstdint>
#include <istream>
#include <mutex>
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

    void write_line(const std::string& line);
    void stop_search();
    [[nodiscard]] bool set_position(std::string_view arguments);
    [[nodiscard]] bool set_option(std::string_view arguments);
    [[nodiscard]] bool start_search(std::string_view arguments);
    void reset_position();
};

}  // namespace blaze

#endif  // BLAZE_UCI_SESSION_H
