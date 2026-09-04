#include "Test.hpp"
#include <iostream>

int main() {
  int failed = 0;
  for (const auto& [name, fn] : test::cases()) { try { fn(); std::cout << "[PASS] " << name << '\n'; } catch (const std::exception& e) { ++failed; std::cerr << "[FAIL] " << name << ": " << e.what() << '\n'; } }
  std::cout << test::cases().size() - failed << "/" << test::cases().size() << " passed\n"; return failed ? 1 : 0;
}

