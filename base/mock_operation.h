#pragma once

#include <cstdint>
#include <string_view>

namespace vela {

// sdk:mock 的私有宿主契约；序号与 SDK 声明共同参与编译内容指纹。
enum class MockOperation : std::uint8_t {
  IsVoid,
  AcceptsReturn,
  Arity,
  ParameterName,
  AcceptsAnswer,
  AcceptsVoidAnswer,
  IsThrowable,
  CaptureTrace,
  ResolveTrace,
  RegisterFailure,
  ObserveMatch,
  FailObservation,
  AcceptsResponseSignature,
};

enum class MockFailureKind : std::uint8_t {
  Usage,
  MissingStub,
  StubExhausted,
  Verification,
  Reset,
  SessionClosed,
};

namespace mock_fields {
inline constexpr std::string_view kDetails = "mock";
inline constexpr std::string_view kSession = "mockSessionID";
inline constexpr std::string_view kMock = "mockID";
inline constexpr std::string_view kMethod = "method";
inline constexpr std::string_view kExpected = "expected";
inline constexpr std::string_view kCount = "actualCount";
inline constexpr std::string_view kCalls = "calls";
inline constexpr std::string_view kSequence = "sequence";
inline constexpr std::string_view kArguments = "arguments";
inline constexpr std::string_view kOmittedArguments = "omittedArguments";
inline constexpr std::string_view kTrace = "trace";
inline constexpr std::string_view kConfigurations = "configurations";
inline constexpr std::string_view kConfigurationTraces = "configurationTraces";
inline constexpr std::string_view kVerificationTrace = "verificationTrace";
}  // namespace mock_fields

}  // namespace vela
