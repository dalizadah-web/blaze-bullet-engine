#include "blaze/uci/session.h"

#include "test_support.h"

#include <sstream>
#include <string>

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
    CHECK(transcript.find("option name UseNNUE type check default false") != std::string::npos);
    CHECK(transcript.find("uciok") != std::string::npos);
    CHECK(transcript.find("readyok") != std::string::npos);
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
