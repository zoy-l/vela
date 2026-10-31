#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "runtime/async/future.h"

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
  struct MockCall final {
    std::int64_t sequence = 0;
    std::vector<std::string> arguments;
    std::int64_t omitted_arguments = 0;
    std::vector<runtime::FutureTraceFrame> trace;
  };
  struct MockDetails final {
    std::int64_t session = 0;
    std::int64_t mock = 0;
    std::string method;
    std::string expected;
    std::int64_t actual_count = 0;
    std::vector<MockCall> calls;
    std::vector<std::string> configurations;
    std::vector<std::vector<runtime::FutureTraceFrame>> configuration_traces;
    std::vector<runtime::FutureTraceFrame> verification_trace;
  };
  std::optional<MockDetails> mock = std::nullopt;
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
