#pragma once

#include <iosfwd>
#include <span>

namespace vela::compiler::aot {

int BuildCommand(std::span<char*> arguments, std::ostream& errors);

}  // namespace vela::compiler::aot
