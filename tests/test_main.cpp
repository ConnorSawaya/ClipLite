#include "test_main.h"

int g_failures = 0;
int g_checks = 0;

std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> r;
    return r;
}

int main() {
    int passed = 0;
    for (auto& tc : test_registry()) {
        const int before = g_failures;
        tc.fn();
        if (g_failures == before) {
            ++passed;
            std::printf("[PASS] %s\n", tc.name);
        } else {
            std::printf("[FAIL] %s\n", tc.name);
        }
    }
    std::printf("\n%d/%zu tests passed, %d checks, %d failures\n", passed,
                test_registry().size(), g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
