#pragma once

#include <expected>
#include <functional>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "compiler/testing/settings.h"

namespace vela::runtime {
class Isolate;
class NativeInstance;
}

namespace vela::testing {

struct CommandStreams final {
  CommandStreams(std::ostream& standard_output, TerminalMode mode, std::ostream& standard_errors)
      : output(standard_output), errors(standard_errors), terminal(mode) {}

  std::reference_wrapper<std::ostream> output;
  std::reference_wrapper<std::ostream> errors;
  TerminalMode terminal;
};
// 每个 suite 独占宿主资源；销毁发生在脚本实例之后、Isolate 之前。
class SuiteHost {
 public:
  SuiteHost() = default;
  virtual ~SuiteHost() = default;
  SuiteHost(const SuiteHost&) = delete;
  SuiteHost& operator=(const SuiteHost&) = delete;
  SuiteHost(SuiteHost&&) = delete;
  SuiteHost& operator=(SuiteHost&&) = delete;
};

// 在实例化前登记绑定并附加 HostData。工厂可被不同 suite 线程并发调用。
using SuiteHostFactory =
    std::function<std::expected<std::unique_ptr<SuiteHost>, std::string>(runtime::Isolate&)>;

[[nodiscard]] int TestCommand(std::span<const std::string_view> arguments,
                              CommandStreams streams,
                              const SuiteHostFactory& create_host = {});
[[nodiscard]] bool HasTestModule(const runtime::NativeInstance& instance);
[[nodiscard]] int RunDirectSuite(runtime::NativeInstance& instance,
                                 std::string_view file,
                                 CommandStreams streams);

}  // namespace vela::testing
