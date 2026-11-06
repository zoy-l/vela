#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "base/callable.h"
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
struct GeneratedFunction;
enum class NativeFrameRecovery : std::uint8_t { None, Required };
struct NativeFrameLayout final {
  llvm::StructType* type = nullptr;
  std::uint64_t bytes = 0;
  std::vector<std::optional<unsigned>> local_fields;
  unsigned temporary_field = 0;
  std::optional<unsigned> leaf_result_field;
};
std::size_t PlanTemporaryCount(const ir::Function& source,
                               const ir::Program& program,
                               const NativeRootPlan& roots);
NativeFrameLayout PlanNativeFrame(llvm::Module& module,
                                  const ir::Program& program,
                                  const EscapePlan& escape,
                                  const NativeRootPlan& roots,
                                  std::size_t temporary_count,
                                  NativeFrameRecovery recovery,
                                  NativeEntryKind kind,
                                  std::span<const GeneratedFunction> generated,
                                  std::span<const std::optional<CallableTarget>> direct_calls);
}  // namespace vela::compiler::native
