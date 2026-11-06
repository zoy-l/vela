#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

enum class EscapeOptimization : std::uint8_t { Disabled, ScalarReplacement, Enabled };

struct AllocationSite final {
  ir::ClassId type = 0;
  bool can_scalar_replace = true;
  std::vector<ir::RegisterId> fields;
  ir::RegisterId reference = 0;
  bool materialized = false;
  bool stack_record = false;
  std::size_t state_offset = 0;
};

struct EscapeFieldSnapshot final {
  DeclarationId declaration;
  ir::RegisterId value = 0;
  std::optional<std::size_t> object;
  bool initialized = false;
};

struct EscapeObjectSnapshot final {
  std::size_t allocation = 0;
  std::vector<EscapeFieldSnapshot> fields;
};

struct EscapeAlias final {
  ir::RegisterId reg = 0;
  std::size_t allocation = 0;
};

struct EscapeCheckpoint final {
  std::vector<ir::RegisterId> required;
  std::vector<EscapeObjectSnapshot> objects;
  std::vector<EscapeAlias> aliases;
};

// 未改写时借用规范 IR；只有实际改写时才拥有分流副本。
// 生产计划的生命周期由输入 Program 覆盖，源码表、映像和恢复目标继续使用规范 IR。
class EscapeFunction final {
 public:
  explicit EscapeFunction(const ir::Function& source) : function_(std::cref(source)) {}
  [[nodiscard]] const ir::Function& get() const {
    if (const auto* rewritten = std::get_if<ir::Function>(&function_)) {
      return *rewritten;
    }
    return std::get<std::reference_wrapper<const ir::Function>>(function_).get();
  }
  ir::Function& Rewrite();

 private:
  std::variant<std::reference_wrapper<const ir::Function>, ir::Function> function_;
};

struct EscapePlan final {
  explicit EscapePlan(const ir::Function& source) : function(source) {}
  EscapeFunction function;
  std::vector<std::size_t> origins;
  std::vector<AllocationSite> allocations;
  // 下标始终是规范 IR 的指令位置；字段来源使用原生分流新增的局部槽。
  std::vector<EscapeCheckpoint> checkpoints;
  // 分析与重写期间借用上游摘要；返回计划前清空，发射只消费 borrowed_calls。
  std::span<const std::optional<ir::ClassId>> borrowable;
  std::vector<std::optional<std::size_t>> borrowed_calls;
  std::vector<ir::RegisterId> borrowed_fields;
  // 按构造函数体事实改写的调用仍依赖具体版本，供 JIT 下一轮复用规划使用。
  std::vector<std::size_t> body_dependencies;
  std::size_t reference_count = 0;
};

[[nodiscard]] EscapePlan PlanEscapes(const ir::Program& program,
                                     std::size_t function_index,
                                     EscapeOptimization optimization = EscapeOptimization::Enabled,
                                     NativeCodeTier tier = NativeCodeTier::Baseline,
                                     std::span<const std::optional<ir::ClassId>> borrowable = {});

}  // namespace vela::compiler::native
