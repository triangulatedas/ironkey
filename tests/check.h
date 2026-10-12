// SPDX-License-Identifier: GPL-2.0-only
// Minimal test harness: TEST(name) { CHECK(...); CHECK_EQ(a, b); }
#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace check {

struct Failure {
    std::string message;
};

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

template <typename A, typename B>
void expect_eq(const A& a, const B& b, const char* expr_a, const char* expr_b, const char* file, int line)
{
    if (a == b)
        return;
    std::ostringstream os;
    os << file << ":" << line << ": CHECK_EQ(" << expr_a << ", " << expr_b << ")\n"
       << "    left:  " << a << "\n    right: " << b;
    throw Failure{os.str()};
}

inline int run_all()
{
    int failed = 0;
    for (const auto& t : registry()) {
        try {
            t.fn();
            std::cout << "PASS " << t.name << "\n";
        } catch (const Failure& f) {
            ++failed;
            std::cout << "FAIL " << t.name << "\n" << f.message << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << t.name << "\n    unexpected exception: " << e.what() << "\n";
        }
    }
    std::cout << (registry().size() - failed) << "/" << registry().size() << " tests passed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace check

#define CHECK_CONCAT_(a, b) a##b
#define CHECK_CONCAT(a, b) CHECK_CONCAT_(a, b)

#define TEST(name)                                                   \
    static void name();                                              \
    static check::Register CHECK_CONCAT(reg_, name)(#name, name);    \
    static void name()

#define CHECK(expr)                                                                         \
    do {                                                                                    \
        if (!(expr))                                                                        \
            throw check::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) +   \
                                 ": CHECK(" #expr ")"};                                     \
    } while (0)

#define CHECK_EQ(a, b) check::expect_eq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_THROWS_AS(expr, type)                                                         \
    do {                                                                                    \
        bool thrown_ = false;                                                               \
        try {                                                                               \
            expr;                                                                           \
        } catch (const type&) {                                                             \
            thrown_ = true;                                                                 \
        }                                                                                   \
        if (!thrown_)                                                                       \
            throw check::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) +   \
                                 ": expected " #type " from " #expr};                       \
    } while (0)
