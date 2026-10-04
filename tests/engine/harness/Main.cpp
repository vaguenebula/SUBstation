#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <random>

#include "Test.h"

namespace subtest {
namespace {

struct Case {
    const char* name;
    const char* file;
    TestFn fn;
};

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

int failures = 0;
std::filesystem::path currentDir;
std::vector<std::string> context;  // INFO()s in scope

}  // namespace

Registrar::Registrar(const char* name, const char* file, TestFn fn) { registry().push_back({name, file, fn}); }

void fail(const char* file, int line, const std::string& message) {
    ++failures;
    std::cerr << "    " << std::filesystem::path(file).filename().string() << ":" << line << ": " << message;
    for (const std::string& about : context) std::cerr << " [" << about << "]";
    std::cerr << "\n";
}

Info::Info(std::string text) { context.push_back(std::move(text)); }
Info::~Info() { context.pop_back(); }

std::filesystem::path tempDir() {
    if (currentDir.empty()) {
        std::random_device random;
        currentDir = std::filesystem::temp_directory_path() /
                     ("substation-test-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::filesystem::create_directories(currentDir);
    }
    return currentDir;
}

}  // namespace subtest

int main(int argc, char** argv) {
    using namespace subtest;
    std::vector<std::string> filters;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            list = true;
        } else {
            filters.emplace_back(argv[i]);
        }
    }
    auto cases = registry();
    std::stable_sort(cases.begin(), cases.end(),
                     [](const Case& a, const Case& b) { return std::strcmp(a.file, b.file) < 0; });
    int run = 0, failed = 0, skipped = 0;
    for (const Case& test : cases) {
        const std::string name = test.name;
        if (!filters.empty() &&
            std::none_of(filters.begin(), filters.end(), [&](const std::string& f) { return name.find(f) != std::string::npos; }))
            continue;
        if (list) {
            std::cout << std::filesystem::path(test.file).stem().string() << ": " << name << "\n";
            continue;
        }
        ++run;
        const int before = failures;
        const auto start = std::chrono::steady_clock::now();
        std::string skipReason;
        try {
            test.fn();
        } catch (const Skipped& skip) {
            skipReason = skip.reason;
        } catch (const Aborted&) {
        } catch (const std::exception& e) {
            fail(test.file, 0, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            fail(test.file, 0, "unexpected exception");
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!currentDir.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(currentDir, ignored);
            currentDir.clear();
        }
        if (!skipReason.empty()) {
            ++skipped;
            std::cout << "SKIP  " << name << " (" << skipReason << ")\n";
        } else if (failures != before) {
            ++failed;
            std::cout << "FAIL  " << name << "\n";
        } else {
            std::cout << "ok    " << name << " (" << static_cast<int>(ms) << " ms)\n";
        }
    }
    if (!list)
        std::cout << "\n" << run - failed - skipped << " passed, " << failed << " failed, " << skipped << " skipped\n";
    return failed ? 1 : 0;
}
