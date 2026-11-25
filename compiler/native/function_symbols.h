#pragma once

#include <string>
#include <vector>

namespace vela::compiler::native {

struct FunctionEntrySymbols final {
  std::string external;
  std::string reflection;
  std::string typed;
  std::vector<std::string> implementations;
};

}  // namespace vela::compiler::native
