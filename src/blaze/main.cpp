#include "blaze/uci/session.h"

#include <iostream>

int main() {
    blaze::UciSession session(std::cout);
    session.run(std::cin);
    return 0;
}
