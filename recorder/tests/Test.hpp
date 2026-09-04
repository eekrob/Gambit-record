#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test {
using Case = std::pair<std::string, std::function<void()>>;
inline std::vector<Case>& cases() { static std::vector<Case> value; return value; }
struct Registrar { Registrar(std::string name, std::function<void()> fn) { cases().emplace_back(std::move(name), std::move(fn)); } };
inline void require(bool value, const char* expression) { if (!value) throw std::runtime_error(std::string("requirement failed: ") + expression); }
}
#define TEST_JOIN2(a,b) a##b
#define TEST_JOIN(a,b) TEST_JOIN2(a,b)
#define TEST_CASE(name) static void TEST_JOIN(test_fn_, __LINE__)(); static test::Registrar TEST_JOIN(test_reg_, __LINE__)(name, TEST_JOIN(test_fn_, __LINE__)); static void TEST_JOIN(test_fn_, __LINE__)()
#define REQUIRE(expr) test::require(static_cast<bool>(expr), #expr)

