#pragma once

#include <string>

#include "base/streams.h"
#include "compiler/jit/compiler.h"
namespace vela::tooling {

[[nodiscard]] int RunDevelopmentConsole(std::string entry,
                                        Streams streams,
                                        compiler::jit::CompileOptions compilation);

}  // namespace vela::tooling
