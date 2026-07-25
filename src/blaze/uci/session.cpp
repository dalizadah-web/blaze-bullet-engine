#include "blaze/uci/session.h"

#include "blaze/core/movegen.h"
#include "blaze/eval/network.h"
#include "blaze/search/search.h"
#include "blaze/uci/limits.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <sstream>
#include <utility>

namespace blaze {
namespace {

constexpr std::string_view start_fen =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

std::vector<std::string> split(std::string_view text) {
    std::istringstream stream{std::string(text)};
    std::vector<std::string> result;
    for (std::string token; stream >> token;) {
        result.push_back(std::move(token));
    }
    return result;
}

std::string trim(std::string_view line) {
    const std::size_t begin = line.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    const std::size_t end = line.find_last_not_of(" \t\r\n");
    std::string result(line.substr(begin, end - begin + 1));
    if (result.size() >= 3 &&
        static_cast<unsigned char>(result[0]) == 0xEF &&
        static_cast<unsigned char>(result[1]) == 0xBB &&
        static_cast<unsigned char>(result[2]) == 0xBF) {
        result.erase(0, 3);
    }
    return result;
}

}  // namespace

UciSession::UciSession(std::ostream& output) : output_(output) {
    reset_position();
}

UciSession::~UciSession() {
    stop_search();
}

void UciSession::run(std::istream& input) {
    for (std::string line; std::getline(input, line);) {
        const std::string command = trim(line);
        static_cast<void>(process_line(command));
        if (command == "quit") {
            break;
        }
    }
    stop_search();
}

bool UciSession::process_line(std::string_view raw_line) {
    const std::string line = trim(raw_line);
    const std::size_t separator = line.find_first_of(" \t");
    const std::string command = line.substr(0, separator);
    const std::string arguments = separator == std::string::npos
        ? std::string{}
        : trim(std::string_view(line).substr(separator + 1));

    if (command.empty()) return true;
    if (command == "uci") {
        write_line("id name Blaze 0.1 clean-room");
        write_line("id author Blaze project");
        write_line("option name Hash type spin default 16 min 1 max 65536");
        write_line("option name Threads type spin default 1 min 1 max 8");
        write_line("option name Move Overhead type spin default 30 min 0 max 1000");
        write_line("option name Ponder type check default false");
        write_line("option name UseNNUE type check default false");
        write_line("option name EvalFile type string default <empty>");
        write_line("uciok");
        return true;
    }
    if (command == "isready") {
        write_line("readyok");
        return true;
    }
    if (command == "ucinewgame") {
        stop_search();
        table_.clear();
        reset_position();
        return true;
    }
    if (command == "position") return set_position(arguments);
    if (command == "setoption") return set_option(arguments);
    if (command == "go") return start_search(arguments);
    if (command == "ponderhit") {
        return handle_ponderhit();
    }
    if (command == "stop") {
        stop_search();
        return true;
    }
    if (command == "quit") {
        stop_search();
        return true;
    }
    write_line("info string unknown command: " + command);
    return false;
}

void UciSession::write_line(const std::string& line) {
    std::lock_guard lock(output_mutex_);
    output_ << line << '\n';
    output_.flush();
}

void UciSession::stop_search() {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) {
        worker_.join();
    }
    stop_requested_.store(false, std::memory_order_relaxed);
}

bool UciSession::set_position(std::string_view arguments) {
    const std::vector<std::string> tokens = split(arguments);
    if (tokens.empty()) {
        write_line("info string position requires startpos or fen");
        return false;
    }

    std::size_t index = 0;
    std::optional<Position> parsed;
    if (tokens[index] == "startpos") {
        parsed = Position::from_fen(start_fen);
        ++index;
    } else if (tokens[index] == "fen") {
        if (tokens.size() < 7) {
            write_line("info string position fen requires six fields");
            return false;
        }
        std::string fen;
        for (std::size_t field = 0; field < 6; ++field) {
            if (field != 0) fen.push_back(' ');
            fen += tokens[++index];
        }
        ++index;
        parsed = Position::from_fen(fen);
    } else {
        write_line("info string position requires startpos or fen");
        return false;
    }
    if (!parsed) {
        write_line("info string invalid FEN");
        return false;
    }

    Position proposed = *parsed;
    std::vector<std::uint64_t> proposed_history{proposed.key()};
    if (index < tokens.size()) {
        if (tokens[index] != "moves") {
            write_line("info string unexpected position token: " + tokens[index]);
            return false;
        }
        ++index;
    }
    for (; index < tokens.size(); ++index) {
        MoveList legal;
        generate_legal(proposed, legal);
        Move selected;
        for (const Move move : legal) {
            if (move_to_uci(move) == tokens[index]) {
                selected = move;
                break;
            }
        }
        StateInfo state;
        if (!selected.is_valid() || !proposed.make_move(selected, state)) {
            write_line("info string illegal position move: " + tokens[index]);
            return false;
        }
        proposed_history.push_back(proposed.key());
    }

    stop_search();
    pondering_ = false;
    position_ = proposed;
    history_ = std::move(proposed_history);
    return true;
}

bool UciSession::set_option(std::string_view arguments) {
    const std::vector<std::string> tokens = split(arguments);
    if (tokens.size() == 4 && tokens[0] == "name" && tokens[1] == "Hash" && tokens[2] == "value") {
        std::size_t megabytes = 0;
        const auto parsed = std::from_chars(tokens[3].data(), tokens[3].data() + tokens[3].size(), megabytes);
        if (parsed.ec != std::errc{} || parsed.ptr != tokens[3].data() + tokens[3].size() ||
            megabytes < 1 || megabytes > 65536) {
            write_line("info string Hash must be between 1 and 65536 MB");
            return false;
        }
        stop_search();
        table_.resize(megabytes);
        return true;
    }
    if (tokens.size() == 4 && tokens[0] == "name" && tokens[1] == "Threads" &&
        tokens[2] == "value") {
        int threads = 0;
        const auto parsed = std::from_chars(tokens[3].data(), tokens[3].data() + tokens[3].size(), threads);
        if (parsed.ec != std::errc{} || parsed.ptr != tokens[3].data() + tokens[3].size() ||
            threads < 1 || threads > 8) {
            write_line("info string Threads must be between 1 and 8");
            return false;
        }
        stop_search();
        threads_ = threads;
        return true;
    }
    if (tokens.size() == 5 && tokens[0] == "name" && tokens[1] == "Move" &&
        tokens[2] == "Overhead" && tokens[3] == "value") {
        int overhead = 0;
        const auto parsed = std::from_chars(
            tokens[4].data(), tokens[4].data() + tokens[4].size(), overhead);
        if (parsed.ec != std::errc{} || parsed.ptr != tokens[4].data() + tokens[4].size() ||
            overhead < 0 || overhead > 1000) {
            write_line("info string Move Overhead must be between 0 and 1000 ms");
            return false;
        }
        move_overhead_ = std::chrono::milliseconds(overhead);
        return true;
    }
    if (tokens.size() == 4 && tokens[0] == "name" && tokens[1] == "Ponder" &&
        tokens[2] == "value" && (tokens[3] == "true" || tokens[3] == "false")) {
        return true;
    }
    if (tokens.size() == 4 && tokens[0] == "name" && tokens[1] == "UseNNUE" &&
        tokens[2] == "value" && (tokens[3] == "true" || tokens[3] == "false")) {
        stop_search();
        use_nnue_ = tokens[3] == "true";
        if (!use_nnue_) network_evaluator_.reset();
        return true;
    }
    if (tokens.size() >= 4 && tokens[0] == "name" && tokens[1] == "EvalFile" &&
        tokens[2] == "value") {
        stop_search();
        network_evaluator_.reset();
        eval_file_.clear();
        for (std::size_t index = 3; index < tokens.size(); ++index) {
            if (!eval_file_.empty()) eval_file_.push_back(' ');
            eval_file_ += tokens[index];
        }
        return true;
    }
    write_line("info string unsupported setoption");
    return false;
}

bool UciSession::start_search(std::string_view arguments) {
    std::string error;
    const auto go = parse_go(arguments, error);
    if (!go) {
        write_line("info string " + error);
        return false;
    }

    stop_search();
    pondering_ = go->ponder;
    ponder_arguments_ = std::string(arguments);
    const Position root = position_;
    if (use_nnue_ && !network_evaluator_) {
        std::string error;
        network_evaluator_ = NetworkEvaluator::create(eval_file_, error);
        if (!network_evaluator_) {
            pondering_ = false;
            write_line("info string critical NNUE unavailable: " + error);
            write_line("bestmove 0000");
            return false;
        }
    } else if (!use_nnue_) {
        network_evaluator_.reset();
    }
    SearchLimits limits = to_search_limits(
        *go,
        root.side_to_move(),
        LatencyBudget{move_overhead_, std::chrono::milliseconds(0)},
        static_cast<int>(history_.empty() ? 0 : history_.size() - 1));
    limits.threads = threads_;
    if (!limits.search_moves.empty()) {
        MoveList legal;
        Position validation = root;
        generate_legal(validation, legal);
        for (const Move requested : limits.search_moves) {
            const bool found = std::any_of(legal.begin(), legal.end(), [&](Move candidate) {
                return candidate.from() == requested.from() &&
                    candidate.to() == requested.to() &&
                    candidate.promotion() == requested.promotion();
            });
            if (!found) {
                write_line("info string searchmoves contains an illegal move");
                return false;
            }
        }
    }
    std::vector<std::uint64_t> prior = history_;
    if (!prior.empty() && prior.back() == root.key()) {
        prior.pop_back();
    }
    worker_ = std::thread([this, root, limits, prior = std::move(prior)]() mutable {
        const auto started = std::chrono::steady_clock::now();
        const NetworkEvaluator* network = network_evaluator_
            ? &*network_evaluator_
            : nullptr;
        Searcher searcher(table_, network);
        const SearchResult result = searcher.search(root, limits, &stop_requested_, prior);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);

        if (suppress_result_.load(std::memory_order_relaxed)) {
            return;
        }

        std::ostringstream info;
        info << "info depth " << result.depth << " score ";
        if (result.score >= search_mate_threshold) {
            info << "mate " << (search_mate_score - result.score + 1) / 2;
        } else if (result.score <= -search_mate_threshold) {
            info << "mate -" << (search_mate_score + result.score + 1) / 2;
        } else {
            info << "cp " << result.score;
        }
        info << " nodes " << result.nodes << " nps ";
        const auto elapsed_ms = std::max<std::int64_t>(elapsed.count(), 1);
        info << (result.nodes * 1000U) / static_cast<std::uint64_t>(elapsed_ms);
        info << " hashfull " << table_.hashfull() << " time " << elapsed.count();
        if (!result.pv.empty()) {
            info << " pv";
            for (const Move move : result.pv) info << ' ' << move_to_uci(move);
        }
        write_line(info.str());
        write_line("bestmove " + move_to_uci(result.best_move));
    });
    return true;
}

bool UciSession::handle_ponderhit() {
    if (!pondering_) {
        return true;
    }

    suppress_result_.store(true, std::memory_order_relaxed);
    stop_search();
    suppress_result_.store(false, std::memory_order_relaxed);
    pondering_ = false;

    const std::vector<std::string> tokens = split(ponder_arguments_);
    std::ostringstream normal_arguments;
    for (const std::string& token : tokens) {
        if (token == "ponder") continue;
        if (normal_arguments.tellp() > 0) normal_arguments << ' ';
        normal_arguments << token;
    }
    if (normal_arguments.tellp() == 0) {
        // A bare `go ponder` has no clock or finite bound. After ponderhit we
        // still need a deterministic finite handoff for UCI clients that use
        // it as a lifecycle probe.
        normal_arguments << "depth 1";
    }
    return start_search(normal_arguments.str());
}

void UciSession::reset_position() {
    position_ = *Position::from_fen(start_fen);
    history_ = {position_.key()};
}

}  // namespace blaze
