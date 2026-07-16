#include "test_support.h"

TEST_CASE(harness_accepts_passing_checks) {
    CHECK(true);
}

int main() {
    return blaze::test::run_all();
}
