#pragma once

#include <functional>
#include <iosfwd>

namespace vela::tooling {

struct LanguageServerStreams final {
  std::reference_wrapper<std::istream> input;
  std::reference_wrapper<std::ostream> output;
  std::reference_wrapper<std::ostream> errors;
};

// stdio 调用方须先启用 binary 模式；所有协议输出只写 output，日志只写 errors。
[[nodiscard]] int RunLanguageServer(LanguageServerStreams streams);

}  // namespace vela::tooling
