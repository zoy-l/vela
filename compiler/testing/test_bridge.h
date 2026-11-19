#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/frontend/builtins/mock_operation.h"
#include "compiler/frontend/builtins/test_operation.h"

#include "runtime/async/future.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class NominalType;

}  // namespace vela::runtime

namespace vela::testing {

enum class TestOperation : std::uint8_t {
  Test,
  EnterGroup,
  ExitGroup,
  SetUp,
  TearDown,
  SetUpAll,
  TearDownAll,
  AddTearDown,
};

struct TestRegistration final {
  TestOperation operation;
  std::string_view description;
  runtime::Value callback;
  runtime::Value skip;
  runtime::Value tags;
  std::int64_t timeout_mode = -1;
  std::int64_t milliseconds = 0;
  double multiplier = 1.0;
  std::vector<runtime::FutureTraceFrame> trace;
};

// 测试驱动拥有 bridge，通过 HostData 在执行作用域挂接；借用期间只在所属线程调用。
// callback 已由执行后端按真实签名校验，所有值在调用期间由脚本寄存器保根。
class TestBridge {
 public:
  TestBridge() = default;
  virtual ~TestBridge() = default;
  TestBridge(const TestBridge&) = delete;
  TestBridge& operator=(const TestBridge&) = delete;
  TestBridge(TestBridge&&) = delete;
  TestBridge& operator=(TestBridge&&) = delete;
  virtual std::expected<void, std::string> Register(const TestRegistration& registration) = 0;
  virtual void RegisterMockFailure(const runtime::NominalType& type, MockFailureKind kind) = 0;
  virtual std::expected<runtime::Value, std::string> Control(
      TestControl operation,
      std::span<const runtime::Value> arguments,
      std::vector<runtime::FutureTraceFrame> trace) = 0;
};

}  // namespace vela::testing
