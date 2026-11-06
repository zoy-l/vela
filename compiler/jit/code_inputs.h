#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "base/fingerprint.h"
#include "base/native_program_mode.h"
#include "compiler/ir/ir.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/field_access.h"

namespace vela::compiler::jit {

struct FunctionCodeInputs final {
  Fingerprint fingerprint;
  std::vector<std::size_t> dependencies;
};

std::vector<FunctionCodeInputs> DescribeCodeInputs(const ir::Program& program,
                                                   const native::NativeCallPlan& calls,
                                                   const native::FieldTargets& fields,
                                                   NativeCodeTier tier,
                                                   std::uint8_t optimization,
                                                   std::uint8_t debugging,
                                                   std::uint8_t escape);

}  // namespace vela::compiler::jit
