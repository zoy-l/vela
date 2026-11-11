#pragma once

#include <cstdint>
#include <string_view>

namespace vela::runtime {
class NativeInstance;
class Isolate;
}

namespace vela::compiler::test {

inline constexpr std::int64_t kExpectedAnswer = 42;

void ExpectAnswer(runtime::NativeInstance& instance);
void ExpectInteger(runtime::NativeInstance& instance,
                   std::int64_t expected = kExpectedAnswer,
                   std::string_view entry = "main");
void ExpectRunAnswer(std::string_view source);
void ExpectRunAnswer(std::string_view source, runtime::Isolate& isolate);
void ExpectFutureAnswer(std::string_view source, std::int64_t expected);
void ExpectRestoredAnswer(std::string_view source);
void ExpectStableAnswer(runtime::NativeInstance& instance);
void ExpectStableAnswer(std::string_view source, runtime::Isolate& isolate);
void ExpectReleasedAnswer(std::string_view source);
void ExpectRejectedSource(std::string_view source, runtime::Isolate& isolate);
void ExpectRejectedSource(std::string_view source);
void ExpectRejectedCheck(std::string_view source);

}  // namespace vela::compiler::test
