// Fabric Evolution — first-party test harness.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Small, dependency-free harness. There are no per-test timeouts: a hanging
// test is a defect to diagnose, so tests run plainly and are allowed to finish.

#pragma once

#include <cstdint>
#include <exception>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "fabric/evolution/status.hpp"

namespace fabric::evolution::test {

class Failure : public std::exception {
 public:
  explicit Failure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

struct TestCase {
  std::string suite;
  std::string name;
  void (*body)();
};

class Registry {
 public:
  [[nodiscard]] static Registry& instance();
  bool add(std::string suite, std::string name, void (*body)());
  [[nodiscard]] const std::vector<TestCase>& cases() const noexcept { return cases_; }

 private:
  std::vector<TestCase> cases_;
};

struct RunOptions {
  std::string filter;
  bool list_only = false;
  bool verbose = false;
  unsigned repeat = 1;
};

[[nodiscard]] int run_all(const RunOptions& options);

// ---------------------------------------------------------------------------
// Value description for failure messages.
// ---------------------------------------------------------------------------
template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <class T>
[[nodiscard]] std::string describe_value(const T& value) {
  if constexpr (is_streamable<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else if constexpr (std::is_enum_v<T>) {
    std::ostringstream stream;
    stream << "enum(" << static_cast<long long>(value) << ")";
    return stream.str();
  } else {
    return "<unprintable>";
  }
}

[[nodiscard]] inline std::string describe_value(const std::string& value) { return "\"" + value + "\""; }
[[nodiscard]] inline std::string describe_value(const char* value) {
  return std::string("\"") + (value == nullptr ? "<null>" : value) + "\"";
}
[[nodiscard]] inline std::string describe_value(bool value) { return value ? "true" : "false"; }
[[nodiscard]] inline std::string describe_value(const Status& value) { return value.to_string(); }

[[nodiscard]] inline const Status& status_of(const Status& value) { return value; }


template <class T>
[[nodiscard]] const Status& status_of(const Result<T>& value) {
  return value.status();
}

template <class T>
[[nodiscard]] std::string describe_status_like(const T& value) {
  if (value.ok()) {
    return "ok";
  }
  return status_of(value).to_string();
}

// ---------------------------------------------------------------------------
// Seeded deterministic randomness for property and state-machine tests.
// ---------------------------------------------------------------------------
class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

  [[nodiscard]] std::uint64_t next_u64() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

  [[nodiscard]] std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next_u64() % bound; }

  [[nodiscard]] bool coin() { return (next_u64() & 1ULL) != 0ULL; }

 private:
  std::uint64_t state_;
};

[[nodiscard]] inline std::string location(const char* file, int line) {
  std::string text(file);
  const std::size_t slash = text.find_last_of("/\\");
  if (slash != std::string::npos) {
    text = text.substr(slash + 1);
  }
  return text + ":" + std::to_string(line);
}

}  // namespace fabric::evolution::test

#define FABRIC_TEST(suite_name, test_name)                                                     \
  static void suite_name##_##test_name##_body();                                               \
  namespace {                                                                                  \
  const bool suite_name##_##test_name##_registered =                                           \
      ::fabric::evolution::test::Registry::instance().add(#suite_name, #test_name,             \
                                                          &suite_name##_##test_name##_body);   \
  }                                                                                            \
  static void suite_name##_##test_name##_body()

#define FABRIC_FAIL(message)                                                                   \
  throw ::fabric::evolution::test::Failure(std::string(message) + " at " +                     \
                                           ::fabric::evolution::test::location(__FILE__, __LINE__))

#define FABRIC_CHECK(expr)                                                                     \
  do {                                                                                         \
    if (!(expr)) {                                                                             \
      FABRIC_FAIL(std::string("check failed: ") + #expr);                                      \
    }                                                                                          \
  } while (false)

#define FABRIC_CHECK_EQ(actual, expected)                                                      \
  do {                                                                                         \
    const auto fabric_actual_ = (actual);                                                      \
    const auto fabric_expected_ = (expected);                                                  \
    if (!(fabric_actual_ == fabric_expected_)) {                                               \
      FABRIC_FAIL(std::string("check failed: ") + #actual + " == " + #expected +               \
                  " (actual=" + ::fabric::evolution::test::describe_value(fabric_actual_) +    \
                  ", expected=" + ::fabric::evolution::test::describe_value(fabric_expected_) + \
                  ")");                                                                        \
    }                                                                                          \
  } while (false)

#define FABRIC_CHECK_NE(actual, unexpected)                                                    \
  do {                                                                                         \
    const auto fabric_actual_ = (actual);                                                      \
    const auto fabric_unexpected_ = (unexpected);                                              \
    if (fabric_actual_ == fabric_unexpected_) {                                                \
      FABRIC_FAIL(std::string("check failed: ") + #actual + " != " + #unexpected +             \
                  " (both=" + ::fabric::evolution::test::describe_value(fabric_actual_) + ")"); \
    }                                                                                          \
  } while (false)

#define FABRIC_CHECK_OK(expr)                                                                  \
  do {                                                                                         \
    auto&& fabric_result_ = (expr);                                                            \
    if (!fabric_result_.ok()) {                                                                \
      FABRIC_FAIL(std::string("expected success from: ") + #expr + " -> " +                    \
                  ::fabric::evolution::test::describe_status_like(fabric_result_));            \
    }                                                                                          \
  } while (false)

#define FABRIC_CHECK_ERR(expr, expected_code)                                                  \
  do {                                                                                         \
    auto&& fabric_result_ = (expr);                                                            \
    if (fabric_result_.ok()) {                                                                 \
      FABRIC_FAIL(std::string("expected failure from: ") + #expr + " but it succeeded");       \
    }                                                                                          \
    const ::fabric::evolution::Status& fabric_status_ =                                        \
        ::fabric::evolution::test::status_of(fabric_result_);                                  \
    if (fabric_status_.code() != (expected_code)) {                                            \
      FABRIC_FAIL(std::string("expected ") + #expected_code + " from " + #expr + " but got " + \
                  std::string(::fabric::evolution::to_string(fabric_status_.code())) + " (" +   \
                  fabric_status_.message() + ")");                                             \
    }                                                                                          \
  } while (false)
