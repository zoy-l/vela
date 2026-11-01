#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "base/native_program_mode.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::jit {

struct FunctionCodeInputs final {
  std::string fingerprint;
  std::vector<std::size_t> dependencies;
};

std::vector<FunctionCodeInputs> DescribeCodeInputs(const ir::Program& program,
                                                   NativeCodeTier tier,
                                                   std::uint8_t optimization,
                                                   std::uint8_t debugging,
                                                   std::uint8_t escape);

}  // namespace vela::compiler::jit
