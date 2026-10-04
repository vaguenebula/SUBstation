#pragma once
// A small test harness for the engine's tests. They link the engine and nothing
// else (no Qt), which also shows that the engine stands on its own.
//
//   TEST_CASE("a clip plays where it starts") { ... CHECK(x == 1); REQUIRE(ok); }
//
// CHECK records a failure and goes on; REQUIRE ends the test. SKIP(reason) ends
// it as skipped. Run `engine_tests [substring...]` to run only the tests whose
// names contain one of the substrings; `--list` lists them.
//
// The Python tests' comparisons: CHECK_APPROX is pytest.approx (relative 1e-6),
// CHECK_APPROX_REL and CHECK_NEAR are approx(rel=...) and approx(abs=...), and
// CHECK_APPROX_TOL is approx(rel=..., abs=...) (either may hold).
// CHECK_THROWS_MATCHING is pytest.raises(..., match=...): the message is searched
// with the regular expression. INFO(text) adds what a failure in its scope was
// about (a parameter, a seed) to the failure's message.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <regex>
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

// pytest.approx's tolerance: the larger of `rel` times the expected value and
// `abs`; with only `abs` given (rel < 0), just that.
inline double approxTolerance(double expected, double rel, double abs) {
    if (rel < 0.0) return abs;
    return std::fmax(rel * std::fabs(expected), abs);
}

// What a failure in its scope was about (INFO).
class Info {
public:
    explicit Info(std::string text);
    ~Info();
    Info(const Info&) = delete;
    Info& operator=(const Info&) = delete;
};

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
        const auto subtest_a = (a); /* (copies: `a` may be a member of a temporary) */                   \
        const auto subtest_b = (b);                                                                       \
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

#define CHECK_APPROX_TOL(a, b, rel, abs_)                                                                \
    do {                                                                                                  \
        const double subtest_a = static_cast<double>(a);                                                  \
        const double subtest_b = static_cast<double>(b);                                                  \
        if (!(std::fabs(subtest_a - subtest_b) <= ::subtest::approxTolerance(subtest_b, rel, abs_)))      \
            ::subtest::fail(__FILE__, __LINE__,                                                           \
                            "CHECK_APPROX(" #a ", " #b "): " + ::subtest::show(subtest_a) + " vs " +       \
                                ::subtest::show(subtest_b));                                              \
    } while (0)
#define CHECK_APPROX(a, b) CHECK_APPROX_TOL(a, b, 1e-6, 1e-12)
#define CHECK_APPROX_REL(a, b, rel) CHECK_APPROX_TOL(a, b, rel, 1e-12)

#define SUBTEST_INFO_NAME SUBTEST_CAT(subtest_info_, __LINE__)
#define INFO(text) const ::subtest::Info SUBTEST_INFO_NAME(text)

#define CHECK_THROWS_MATCHING(expr, Type, pattern)                                                   \
    do {                                                                                             \
        std::string subtest_message;                                                                 \
        bool subtest_threw = false;                                                                  \
        try {                                                                                        \
            (void)(expr);                                                                            \
        } catch (const Type& subtest_e) {                                                            \
            subtest_threw = true;                                                                    \
            subtest_message = subtest_e.what();                                                      \
        } catch (...) {                                                                              \
        }                                                                                            \
        if (!subtest_threw)                                                                          \
            ::subtest::fail(__FILE__, __LINE__, "CHECK_THROWS_MATCHING(" #expr ", " #Type "): no " #Type); \
        else if (!std::regex_search(subtest_message, std::regex(pattern)))                           \
            ::subtest::fail(__FILE__, __LINE__,                                                      \
                            "CHECK_THROWS_MATCHING(" #expr "): \"" + subtest_message +              \
                                "\" doesn't match \"" + std::string(pattern) + "\"");               \
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
