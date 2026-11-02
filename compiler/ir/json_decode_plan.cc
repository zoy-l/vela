#include "compiler/ir/ir.h"

#include <cstdint>
#include <vector>

#include "base/json_decode.h"

namespace vela::ir {

std::vector<std::uint32_t> JSONDecodeClosure(const Program& program, std::uint32_t root) {
  std::vector<std::uint32_t> result;
  std::vector<std::uint32_t> pending{root};
  std::vector<bool> seen(program.json_decode_plans.size());
  while (!pending.empty()) {
    const auto index = pending.back();
    pending.pop_back();
    if (seen.at(index)) {
      continue;
    }
    seen.at(index) = true;
    result.push_back(index);
    const auto& plan = program.json_decode_plans.at(index);
    if (plan.kind == JSONDecodeKind::Nullable || plan.kind == JSONDecodeKind::Array ||
        plan.kind == JSONDecodeKind::Map) {
      pending.push_back(plan.element);
    }
    for (const auto& parameter : plan.parameters) {
      pending.push_back(parameter.node);
    }
  }
  return result;
}

}  // namespace vela::ir
