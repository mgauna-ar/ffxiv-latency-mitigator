#include "test_framework.hpp"
#include <iostream>
#include <iomanip>

int main() {
    const auto& tests = mitigator::test::TestRegistry::instance().tests();

    std::cout << "\n=======================================================\n";
    std::cout << "  RUNNING FFXIV LATENCY MITIGATOR UNIT TEST SUITE\n";
    std::cout << "  Total Test Cases Registered: " << tests.size() << "\n";
    std::cout << "=======================================================\n\n";

    size_t passed = 0;
    size_t failed = 0;

    for (const auto& test : tests) {
        std::cout << "  [RUN]  " << std::setw(18) << std::left << test.suite
                  << " :: " << test.name << " ... ";
        std::cout.flush();

        try {
            test.func();
            std::cout << "\033[32mPASSED\033[0m\n";
            ++passed;
        } catch (const std::exception& ex) {
            std::cout << "\033[31mFAILED\033[0m\n";
            std::cerr << "         \033[31mError: " << ex.what() << "\033[0m\n";
            ++failed;
        } catch (...) {
            std::cout << "\033[31mFAILED (Unknown exception)\033[0m\n";
            ++failed;
        }
    }

    std::cout << "\n-------------------------------------------------------\n";
    std::cout << "Test Summary: "
              << "\033[32m" << passed << " Passed\033[0m, "
              << (failed > 0 ? "\033[31m" : "\033[32m") << failed << " Failed\033[0m / "
              << tests.size() << " Total\n";
    std::cout << "-------------------------------------------------------\n\n";

    return (failed == 0) ? 0 : 1;
}
