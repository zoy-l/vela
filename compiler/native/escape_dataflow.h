#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/escape_analysis.h"
namespace vela::compiler::native::internal {
struct Provenance final {
  std::uint64_t objects = 0;
  bool other = true;
  bool initialized = false;
  bool operator==(const Provenance&) const = default;
};
using State = std::vector<Provenance>;
struct BlockState final {
  State values;
  bool reached = false;
};
std::uint64_t MayHeap(const State& state);
std::uint64_t MustHeap(const State& state);
bool IsVirtual(const EscapePlan& plan, std::size_t object);
std::uint64_t Reachable(const EscapePlan& plan, const State& state, std::uint64_t roots);
std::optional<std::size_t> Object(const Provenance& value);
std::optional<std::size_t> ScalarObject(const EscapePlan& plan, const Provenance& value);
std::optional<std::size_t> VirtualReceiver(const EscapePlan& plan,
                                           const ir::Instruction& instruction,
                                           const State& values);
bool MatchesType(const ir::Program& program,
                 const AllocationSite& allocation,
                 std::uint32_t type_index);
bool IsTrivialConstructor(const ir::Function& function);
bool NeedsHeap(const EscapePlan& plan,
               const ir::Program& program,
               const ir::Instruction& instruction,
               const State& values);
std::optional<std::size_t> BorrowedReceiver(const EscapePlan& plan,
                                            const ir::Instruction& instruction,
                                            const State& values);
void Transfer(const ir::Instruction& instruction,
              const ir::RegisterAccess& access,
              std::optional<std::size_t> allocation,
              const EscapePlan& plan,
              const ir::Program& program,
              State& values);
std::vector<BlockState> Solve(const ir::Function& function,
                              const ir::Program& program,
                              const ir::FlowGraph& graph,
                              std::span<const std::optional<std::size_t>> allocations,
                              const EscapePlan& plan,
                              std::size_t state_size,
                              State entry = {});
void RewriteEscapes(EscapePlan& plan,
                    const ir::Program& program,
                    std::span<const std::vector<ir::RegisterId>> live,
                    const ir::FlowGraph& graph,
                    std::span<const BlockState> blocks,
                    std::span<const std::optional<std::size_t>> allocations);
}  // namespace vela::compiler::native::internal
