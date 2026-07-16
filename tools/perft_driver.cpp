#include "blaze/core/perft.h"
#include "blaze/core/perft_request.h"

#include <iostream>
#include <string>

int main() {
    std::string line;
    bool had_error = false;

    while (std::getline(std::cin, line)) {
        auto request = blaze::parse_perft_request(line);
        if (!request.has_value()) {
            std::cout << "error\n";
            had_error = true;
            continue;
        }
        std::cout << blaze::perft(request->position, request->depth) << '\n';
    }

    return had_error ? 1 : 0;
}
