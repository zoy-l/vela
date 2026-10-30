#pragma once

#include <functional>
#include <iosfwd>
#include <string>

#include "compiler/jit/compiler.h"
namespace vela::tooling {

struct DevelopmentStreams final {
  std::reference_wrapper<std::istream> input;
  std::reference_wrapper<std::ostream> output;
  std::reference_wrapper<std::ostream> errors;
};

[[nodiscard]] int RunDevelopmentConsole(std::string entry,
                                        DevelopmentStreams streams,
                                        compiler::jit::CompileOptions compilation = {});

}  // namespace vela::tooling
