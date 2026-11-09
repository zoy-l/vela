#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include "compiler/ir/ir.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/native_roots.h"
namespace llvm {
class Module;
class StructType;
}
namespace vela::compiler::native {
inline constexpr unsigned kVirtualReferenceFieldIndex = 5;
inline constexpr unsigned kVirtualStateFieldIndex = 6;
enum class NativeEntryKind : std::uint8_t;
struct NativeFrameRequirements final {
  bool recovery = false;
  bool leaf_calls = false;
};
struct NativeFrameLayout final {
  llvm::StructType* type = nullptr;
  std::uint64_t bytes = 0;
  std::vector<std::optional<unsigned>> local_fields;
  unsigned temporary_field = 0;
  std::optional<unsigned> leaf_result_field;
  std::optional<unsigned> auxiliary_field;
};
std::size_t PlanTemporaryCount(const ir::Function& source,
                               const ir::Program& program,
                               const NativeRootPlan& roots);
NativeFrameLayout PlanNativeFrame(llvm::Module& module,
                                  const EscapePlan& escape,
                                  const NativeRootPlan& roots,
                                  std::size_t temporary_count,
                                  NativeFrameRequirements requirements,
                                  NativeEntryKind kind,
                                  std::size_t auxiliary_bytes);
}  // namespace vela::compiler::native
