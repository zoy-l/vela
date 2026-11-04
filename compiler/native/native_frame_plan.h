#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
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
std::expected<std::size_t, std::string> PlanTemporaryCount(const ir::Function& source,
                                                           const ir::Program& program,
                                                           const NativeRootPlan& roots);
std::expected<NativeFrameLayout, std::string> PlanNativeFrame(
    llvm::Module& module,
    const ir::Program& program,
    const EscapePlan& escape,
    const NativeRootPlan& roots,
    NativeFrameRecovery recovery,
    NativeEntryKind kind,
    std::span<const GeneratedFunction> generated,
    std::span<const std::optional<CallableTarget>> direct_calls);
}  // namespace vela::compiler::native
