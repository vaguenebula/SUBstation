#pragma once
// A small test harness for the engine's tests. They link the engine and nothing
// else (no Qt), which also shows that the engine stands on its own.
//
//   TEST_CASE("a clip plays where it starts") { ... CHECK(x == 1); REQUIRE(ok); }
//
// CHECK records a failure and goes on; REQUIRE ends the test. SKIP(reason) ends
// it as skipped. Run `engine_tests [substring...]` to run only the tests whose
// names contain one of the substrings; `--list` lists them.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace subtest {

using TestFn = void (*)();

struct Registrar {
    Registrar(const char* name, const char* file, TestFn fn);
};

struct Skipped {
    std::string reason;
};
struct Aborted {};

void fail(const char* file, int line, const std::string& message);

template <typename T>
std::string show(const T& value) {
    std::ostringstream out;
    if constexpr (requires { out << value; }) {
        out.precision(10);
        out << value;
    } else {
        out << "<value>";
    }
    return out.str();
}

// A fresh folder for the running test (removed after it).
std::filesystem::path tempDir();

}  // namespace subtest

#define SUBTEST_CAT2(a, b) a##b
#define SUBTEST_CAT(a, b) SUBTEST_CAT2(a, b)
#define TEST_CASE(name)                                                                                  \
    static void SUBTEST_CAT(subtest_fn_, __LINE__)();                                                   \
    static const ::subtest::Registrar SUBTEST_CAT(subtest_reg_, __LINE__)(name, __FILE__,                \
                                                                        &SUBTEST_CAT(subtest_fn_, __LINE__)); \
    static void SUBTEST_CAT(subtest_fn_, __LINE__)()

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) ::subtest::fail(__FILE__, __LINE__, "CHECK(" #expr ")"); \
    } while (0)

#define REQUIRE(expr)                                                         \
    do {                                                                      \
        if (!(expr)) {                                                        \
            ::subtest::fail(__FILE__, __LINE__, "REQUIRE(" #expr ")");       \
            throw ::subtest::Aborted{};                                       \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        const auto& subtest_a = (a);                                                                      \
        const auto& subtest_b = (b);                                                                      \
        if (!(subtest_a == subtest_b))                                                                    \
            ::subtest::fail(__FILE__, __LINE__,                                                           \
                            "CHECK_EQ(" #a ", " #b "): " + ::subtest::show(subtest_a) + " != " +           \
                                ::subtest::show(subtest_b));                                              \
    } while (0)

#define CHECK_NEAR(a, b, tolerance)                                                                       \
    do {                                                                                                  \
        const double subtest_a = static_cast<double>(a);                                                  \
        const double subtest_b = static_cast<double>(b);                                                  \
        if (!(std::fabs(subtest_a - subtest_b) <= static_cast<double>(tolerance)))                        \
            ::subtest::fail(__FILE__, __LINE__,                                                           \
                            "CHECK_NEAR(" #a ", " #b ", " #tolerance "): " + ::subtest::show(subtest_a) + \
                                " vs " + ::subtest::show(subtest_b));                                     \
    } while (0)

#define CHECK_THROWS_AS(expr, Type)                                                                   \
    do {                                                                                              \
        bool subtest_threw = false;                                                                   \
        try {                                                                                         \
            (void)(expr);                                                                             \
        } catch (const Type&) {                                                                       \
            subtest_threw = true;                                                                     \
        } catch (...) {                                                                               \
        }                                                                                             \
        if (!subtest_threw) ::subtest::fail(__FILE__, __LINE__, "CHECK_THROWS_AS(" #expr ", " #Type ")"); \
    } while (0)

#define CHECK_NOTHROW(expr)                                                                   \
    do {                                                                                      \
        try {                                                                                 \
            (void)(expr);                                                                     \
        } catch (const std::exception& subtest_e) {                                          \
            ::subtest::fail(__FILE__, __LINE__,                                               \
                            std::string("CHECK_NOTHROW(" #expr "): ") + subtest_e.what());   \
        }                                                                                     \
    } while (0)

#define SKIP(reason) throw ::subtest::Skipped{reason}
