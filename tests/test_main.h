#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

template <typename T>
std::string to_str(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& test_registry();

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        test_registry().push_back({name, std::move(fn)});
    }
};

extern int g_failures;
extern int g_checks;

#define TEST(name) \
    static void test_##name(); \
    static Registrar reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) \
    do { \
        ++g_checks; \
        if (!(cond)) { \
            ++g_failures; \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        ++g_checks; \
        auto va = (a); \
        auto vb = (b); \
        if (!(va == vb)) { \
            ++g_failures; \
            std::printf("  FAIL %s:%d: %s == %s  (lhs=%s, rhs=%s)\n", __FILE__, __LINE__, \
                        #a, #b, to_str(va).c_str(), to_str(vb).c_str()); \
        } \
    } while (0)
