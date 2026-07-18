#include "blaze/uci/session.h"

#include "test_support.h"

#include <sstream>
#include <string>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace blaze {
namespace {

std::size_t occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t offset = 0; (offset = text.find(needle, offset)) != std::string::npos;
         offset += needle.size()) {
        ++count;
    }
    return count;
}

TEST_CASE(uci_handshake_and_readiness_are_reported) {
    std::istringstream input("uci\nisready\nquit\n");
    std::ostringstream output;
    UciSession session(output);
    session.run(input);
    const std::string transcript = output.str();
    CHECK(transcript.find("id name Blaze") != std::string::npos);
    CHECK(transcript.find("option name Hash type spin") != std::string::npos);
    CHECK(transcript.find("option name Threads type spin default 1 min 1 max 8") != std::string::npos);
    CHECK(transcript.find("option name Move Overhead type spin default 30 min 0 max 1000") != std::string::npos);
    CHECK(transcript.find("option name UseNNUE type check default false") != std::string::npos);
    CHECK(transcript.find("uciok") != std::string::npos);
    CHECK(transcript.find("readyok") != std::string::npos);
}

void write_zero_network(const std::filesystem::path& path) {
    constexpr std::size_t payload_size = 768U * 256U * 2U + 256U * 4U + 256U * 2U + 4U;
    const std::vector<std::uint8_t> payload(payload_size, 0);
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : payload) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ofstream output(path, std::ios::binary);
    output.write("BLAZENET", 8);
    const std::uint32_t version = 1;
    const std::uint32_t features = 768;
    const std::uint32_t hidden = 256;
    const std::uint32_t bytes = static_cast<std::uint32_t>(payload.size());
    output.write(reinterpret_cast<const char*>(&version), sizeof(version));
    output.write(reinterpret_cast<const char*>(&features), sizeof(features));
    output.write(reinterpret_cast<const char*>(&hidden), sizeof(hidden));
    output.write(reinterpret_cast<const char*>(&bytes), sizeof(bytes));
    output.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
    output.write(reinterpret_cast<const char*>(payload.data()), payload.size());
}

TEST_CASE(move_overhead_option_is_validated_for_clock_safety) {
    std::ostringstream output;
    UciSession session(output);
    CHECK(session.process_line("setoption name Move Overhead value 75"));
    CHECK(!session.process_line("setoption name Move Overhead value 1001"));
    CHECK(output.str().find("Move Overhead must be between 0 and 1000 ms") != std::string::npos);
}

TEST_CASE(required_nnue_reports_critical_failure_instead_of_playing_fallback) {
    std::ostringstream output;
    UciSession session(output);
    CHECK(session.process_line("setoption name UseNNUE value true"));
    CHECK(session.process_line("position startpos"));
    CHECK(!session.process_line("go depth 1"));
    CHECK(output.str().find("info string critical NNUE unavailable") != std::string::npos);
    CHECK(output.str().find("bestmove 0000") != std::string::npos);
}

TEST_CASE(loaded_network_is_reused_across_moves) {
    const std::filesystem::path path = "build/blaze/session-cache.blaze-net";
    write_zero_network(path);

    std::ostringstream output;
    UciSession session(output);
    CHECK(session.process_line("setoption name EvalFile value " + path.string()));
    CHECK(session.process_line("setoption name UseNNUE value true"));
    CHECK(session.process_line("position startpos"));
    CHECK(session.process_line("go depth 1"));
    CHECK(session.process_line("stop"));
    std::filesystem::remove(path);

    CHECK(session.process_line("go depth 1"));
    CHECK(session.process_line("stop"));
    CHECK(output.str().find("critical NNUE unavailable") == std::string::npos);
}

TEST_CASE(threads_option_is_accepted_and_starts_parallel_search) {
    std::ostringstream output;
    UciSession session(output);
    CHECK(session.process_line("setoption name Threads value 2"));
    CHECK(session.process_line("position startpos"));
    CHECK(session.process_line("go depth 2"));
    CHECK(session.process_line("stop"));
    CHECK_EQ(occurrences(output.str(), "bestmove "), 1U);
}

TEST_CASE(uci_handshake_accepts_an_initial_utf8_bom) {
    std::istringstream input("\xEF\xBB\xBF" "uci\nquit\n");
    std::ostringstream output;
    UciSession session(output);
    session.run(input);
    CHECK(output.str().find("uciok") != std::string::npos);
}

TEST_CASE(position_command_applies_only_complete_legal_move_sequences) {
    std::ostringstream output;
    UciSession session(output);
    CHECK(session.process_line("position startpos moves e2e4 e7e5 g1f3"));
    CHECK(session.current_fen().find(" b ") != std::string::npos);
    const std::string accepted = session.current_fen();
    CHECK(!session.process_line("position startpos moves e2e5"));
    CHECK_EQ(session.current_fen(), accepted);
}

TEST_CASE(go_stop_and_quit_emit_exactly_one_bestmove) {
    std::istringstream input("position startpos\ngo infinite\nstop\nquit\n");
    std::ostringstream output;
    UciSession session(output);
    session.run(input);
    CHECK_EQ(occurrences(output.str(), "bestmove "), 1U);
}

TEST_CASE(negative_gui_clock_still_emits_one_legal_bestmove) {
    std::ostringstream output;
    UciSession session(output);
    CHECK(session.process_line("setoption name Move Overhead value 0"));
    CHECK(session.process_line("position startpos"));
    CHECK(session.process_line("go wtime -22 btime 18 winc 1000 binc 1000"));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(session.process_line("isready"));
    CHECK(session.process_line("stop"));

    const std::string transcript = output.str();
    CHECK_EQ(occurrences(transcript, "bestmove "), 1U);
    CHECK(transcript.find("bestmove 0000") == std::string::npos);
    CHECK(transcript.find("invalid value for go wtime") == std::string::npos);
    CHECK(transcript.find("bestmove ") < transcript.find("readyok"));
}

TEST_CASE(repeated_go_replaces_previous_worker_without_duplicate_results) {
    std::istringstream input(
        "position startpos\n"
        "go infinite\n"
        "go nodes 100\n"
        "stop\n"
        "isready\n"
        "quit\n");
    std::ostringstream output;
    UciSession session(output);
    session.run(input);
    CHECK_EQ(occurrences(output.str(), "bestmove "), 2U);
    CHECK(output.str().find("readyok") != std::string::npos);
}

TEST_CASE(ponderhit_transitions_to_a_clocked_search_without_duplicate_bestmove) {
    std::ostringstream output;
    UciSession session(output);
    CHECK(session.process_line("position startpos"));
    CHECK(session.process_line("go ponder wtime 1000 btime 1000"));
    CHECK(session.process_line("ponderhit"));
    CHECK(session.process_line("stop"));
    CHECK_EQ(occurrences(output.str(), "bestmove "), 1U);
}

TEST_CASE(malformed_commands_produce_diagnostics_without_killing_session) {
    std::istringstream input("position fen nonsense\ngo mystery 1\nisready\nquit\n");
    std::ostringstream output;
    UciSession session(output);
    session.run(input);
    CHECK(output.str().find("info string") != std::string::npos);
    CHECK(output.str().find("readyok") != std::string::npos);
}

}  // namespace
}  // namespace blaze
