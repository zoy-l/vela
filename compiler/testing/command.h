#pragma once

#include <functional>
#include <iosfwd>
#include <span>
#include <string_view>

namespace vela::runtime {
class Isolate;
}
namespace vela::vm {
class Program;
}

namespace vela::testing {

struct CommandStreams final {
  std::reference_wrapper<std::ostream> output;
  std::reference_wrapper<std::ostream> errors;
};
[[nodiscard]] int TestCommand(std::span<const std::string_view> arguments, CommandStreams streams);
[[nodiscard]] bool HasTestModule(const vm::Program& program);
[[nodiscard]] int RunDirectSuite(const vm::Program& program,
                                 runtime::Isolate& isolate,
                                 std::string_view file,
                                 CommandStreams streams);

}  // namespace vela::testing
