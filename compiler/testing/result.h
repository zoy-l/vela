#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "runtime/future.h"

namespace vela::testing {

enum class TestPhase : std::uint8_t {
  Registration,
  SetUpAll,
  SetUp,
  Body,
  DynamicTearDown,
  TearDown,
  TearDownAll,
};
enum class TestStatus : std::uint8_t { Pending, Passed, Failed, Skipped, Cancelled };

struct Difference final {
  std::string path;
  std::string actual;
  std::vector<Difference> children;
};

struct TestError final {
  TestPhase phase = TestPhase::Body;
  std::string category;
  std::string message;
  std::vector<runtime::FutureTraceFrame> trace;
  std::optional<Difference> difference;
};

struct TestResult final {
  std::uint64_t id = 0;
  std::string name;
  std::vector<std::string> tags;
  std::string skip_reason;
  std::vector<runtime::FutureTraceFrame> registration_trace;
  std::vector<TestError> errors;
  TestStatus status = TestStatus::Pending;
};

struct SuiteResult final {
  std::vector<TestResult> tests;
  std::vector<TestError> errors;
  int exit_code = 0;
};

}  // namespace vela::testing
