#include "blaze/uci/session.h"
#include "blaze/eval/network.h"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--require-network") {
        std::string error;
        if (!blaze::NetworkEvaluator::create(argv[2], error)) {
            std::cerr << "critical: required NNUE network unavailable: " << error << '\n';
            return 1;
        }
    } else if (argc != 1) {
        std::cerr << "usage: blaze [--require-network PATH]\n";
        return 2;
    }
    blaze::UciSession session(std::cout);
    session.run(std::cin);
    return 0;
}
