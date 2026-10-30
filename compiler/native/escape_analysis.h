#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

enum class EscapeOptimization : std::uint8_t { Disabled, ScalarReplacement, Enabled };

enum class AllocationDecision : std::uint8_t {
  ScalarReplacement,
  NonInstance,
  CallBoundary,
  Publication,
  AmbiguousAlias,
  LoopLifetime,
  UnsupportedUse,
  AnalysisBudget,
  RecoveryRequired,
  OptimizationDisabled,
};

struct AllocationSite final {
  std::size_t instruction = 0;
  ir::ClassId type = 0;
  AllocationDecision decision = AllocationDecision::ScalarReplacement;
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

// 仅原生分流持有此副本；源码表、映像和恢复目标继续使用规范 IR。
struct EscapePlan final {
  ir::Function function;
  std::vector<std::size_t> origins;
  std::vector<AllocationSite> allocations;
  // 下标始终是规范 IR 的指令位置；字段来源使用原生分流新增的局部槽。
  std::vector<EscapeCheckpoint> checkpoints;
  // 分析与重写期间借用上游摘要；返回计划前清空，发射只消费 borrowed_calls。
  std::span<const std::optional<ir::ClassId>> borrowable;
  std::vector<std::optional<std::size_t>> borrowed_calls;
  std::vector<ir::RegisterId> borrowed_fields;
  std::size_t reference_count = 0;
};

[[nodiscard]] EscapePlan PlanEscapes(const ir::Program& program,
                                     std::size_t function_index,
                                     EscapeOptimization optimization = EscapeOptimization::Enabled,
                                     NativeCodeTier tier = NativeCodeTier::Baseline,
                                     std::span<const std::optional<ir::ClassId>> borrowable = {});

}  // namespace vela::compiler::native
