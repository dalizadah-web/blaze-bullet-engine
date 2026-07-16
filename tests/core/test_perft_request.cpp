#include "test_support.h"

#include "blaze/core/perft_request.h"

#include <string>

TEST_CASE(perft_request_parses_fen_and_depth) {
    const auto request = blaze::parse_perft_request(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1\t3");
    CHECK(request.has_value());
    CHECK_EQ(request->depth, 3);
    CHECK_EQ(
        request->position.to_fen(),
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

TEST_CASE(perft_request_accepts_utf8_bom) {
    const std::string input =
        std::string{"\xEF\xBB\xBF"} +
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1\t2";
    const auto request = blaze::parse_perft_request(input);
    CHECK(request.has_value());
    CHECK_EQ(request->depth, 2);
}

TEST_CASE(perft_request_rejects_invalid_lines) {
    CHECK(!blaze::parse_perft_request("missing-tab").has_value());
    CHECK(!blaze::parse_perft_request("not-a-fen\t3").has_value());
    CHECK(!blaze::parse_perft_request(
        "4k3/8/8/8/8/8/8/4K3 w - - 0 1\t-1")
               .has_value());
}
