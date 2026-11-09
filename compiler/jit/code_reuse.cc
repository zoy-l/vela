#include "compiler/jit/code_reuse.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "compiler/jit/code_inputs.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Module.h>

namespace vela::compiler::jit {
namespace {

void InvalidateCallers(const std::vector<std::vector<std::size_t>>& callers,
                       std::vector<std::size_t>& changed,
                       CodeReusePlan& plan) {
  // 只有实际直接引用或函数体特化依赖沿反向边传播；入口槽不会产生版本依赖。
  while (!changed.empty()) {
    const auto index = changed.back();
    changed.pop_back();
    for (const auto caller : callers[index]) {
      if (plan.reused[caller] != 0) {
        plan.reused[caller] = 0;
        changed.push_back(caller);
      }
    }
  }
}

}  // namespace

CodeReusePlan PlanCodeReuse(std::vector<FunctionCodeInputs> inputs, const CodeReuseCache& cache) {
  CodeReusePlan plan;
  plan.inputs = std::move(inputs);
  const auto count = plan.inputs.size();
  plan.reused.resize(count);
  std::vector<std::vector<std::size_t>> callers(count);
  std::vector<std::size_t> changed;
  for (std::size_t index = 0; index < count; ++index) {
    const auto& input = plan.inputs[index];
    const auto* source = index < cache.sources.size() ? &cache.sources[index] : nullptr;
    auto reusable = source != nullptr && source->fingerprint == input.fingerprint;
    if (reusable) {
      for (const auto& name : source->symbols) {
        const auto found = cache.functions.find(name);
        if (found == cache.functions.end()) {
          reusable = false;
          break;
        }
        auto resource = found->second.resource.lock();
        if (!resource) {
          reusable = false;
          break;
        }
        plan.retained.push_back(std::move(resource));
      }
    }
    plan.reused[index] = reusable ? 1 : 0;
    if (!reusable) {
      changed.push_back(index);
    }
    const auto& dependencies = reusable ? source->body_dependencies : input.dependencies;
    for (const auto dependency : dependencies) {
      callers[dependency].push_back(index);
    }
  }
  InvalidateCallers(callers, changed, plan);
  return plan;
}

void BindReusableCode(llvm::Module& module,
                      const CodeReuseCache& cache,
                      const std::vector<std::vector<std::string>>& symbols,
                      std::uint64_t batch,
                      CodeReusePlan& plan) {
  for (std::size_t index = 0; index < symbols.size(); ++index) {
    const auto reused = plan.reused[index] != 0;
    if (!reused) {
      ++plan.compiled_functions;
    }
    for (const auto& name : symbols[index]) {
      ReuseFunction entry{.name = name, .symbol = {}, .resource = {}};
      if (reused) {
        const auto& cached = cache.functions.at(name);
        entry.resource = cached.resource.lock();
        entry.address = cached.address;
        entry.symbol = cached.symbol;
      } else {
        entry.symbol = name + "_batch_" + std::to_string(batch);
      }
      if (auto* function = module.getFunction(name)) {
        if (reused) {
          function->removeFnAttr(llvm::Attribute::AlwaysInline);
        }
        function->setName(entry.symbol);
        function->setLinkage(llvm::GlobalValue::ExternalLinkage);
      }
      plan.functions.push_back(std::move(entry));
    }
  }
}

}  // namespace vela::compiler::jit
