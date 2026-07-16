#ifndef BLAZE_TEST_SUPPORT_H
#define BLAZE_TEST_SUPPORT_H

#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace blaze::test {

struct Case {
    std::string_view name;
    void (*function)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

class Failure final : public std::exception {
public:
    explicit Failure(std::string message) : message_(std::move(message)) {}

    [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

class Registrar {
public:
    Registrar(std::string_view name, void (*function)()) {
        registry().push_back(Case{name, function});
    }
};

[[noreturn]] inline void fail(
    std::string_view expression,
    std::string_view file,
    int line) {
    std::ostringstream message;
    message << file << ':' << line << ": check failed: " << expression;
    throw Failure(message.str());
}

inline int run_all() {
    int passed = 0;
    int failed = 0;

    for (const Case& test_case : registry()) {
        try {
            test_case.function();
            ++passed;
            std::cout << "PASS " << test_case.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << test_case.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "FAIL " << test_case.name << ": unknown exception\n";
        }
    }

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace blaze::test

#define BLAZE_TEST_CONCAT_INNER(left, right) left##right
#define BLAZE_TEST_CONCAT(left, right) BLAZE_TEST_CONCAT_INNER(left, right)

#define TEST_CASE(name)                                                                     \
    static void name();                                                                     \
    static const ::blaze::test::Registrar BLAZE_TEST_CONCAT(blaze_registrar_, __LINE__){    \
        #name, &name};                                                                      \
    static void name()

#define CHECK(expression)                                                                   \
    do {                                                                                    \
        if (!(expression)) {                                                                \
            ::blaze::test::fail(#expression, __FILE__, __LINE__);                           \
        }                                                                                   \
    } while (false)

#define CHECK_EQ(actual, expected)                                                          \
    do {                                                                                    \
        const auto& blaze_actual = (actual);                                                \
        const auto& blaze_expected = (expected);                                            \
        if (!(blaze_actual == blaze_expected)) {                                            \
            ::blaze::test::fail(#actual " == " #expected, __FILE__, __LINE__);             \
        }                                                                                   \
    } while (false)

#endif  // BLAZE_TEST_SUPPORT_H
