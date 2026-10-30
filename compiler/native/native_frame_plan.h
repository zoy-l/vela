#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include "base/native_program_mode.h"
#include "compiler/ir/ir.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/native_roots.h"
namespace llvm {
class Module;
class StructType;
}
namespace vela::compiler::native {
inline constexpr unsigned kVirtualReferenceFieldIndex = 5;
enum class NativeEntryKind : std::uint8_t;
struct NativeFrameLayout final {
  llvm::StructType* type = nullptr;
  std::uint64_t bytes = 0;
};
std::expected<std::size_t, std::string> PlanTemporaryCount(const ir::Function& source,
                                                           const ir::Program& program,
                                                           const NativeRootPlan& roots);
std::expected<NativeFrameLayout, std::string> PlanNativeFrame(llvm::Module& module,
                                                              const ir::Program& program,
                                                              const EscapePlan& escape,
                                                              const NativeRootPlan& roots,
                                                              NativeCodeTier tier,
                                                              NativeEntryKind kind);
}  // namespace vela::compiler::native
