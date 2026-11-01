#include "compiler/jit/code_reuse.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "compiler/jit/code_inputs.h"

#include <absl/container/flat_hash_set.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Module.h>

namespace vela::compiler::jit {
namespace {

void InvalidateCallers(const std::vector<std::vector<std::size_t>>& callers,
                       std::vector<std::size_t>& changed,
                       CodeReusePlan& plan) {
  // 直接调用与内联的版本依赖沿反向边传播，递归调用分量作为整体失效。
  while (!changed.empty()) {
    const auto index = changed.back();
    changed.pop_back();
    for (const auto caller : callers.at(index)) {
      if (plan.reused.at(caller) != 0) {
        plan.reused.at(caller) = 0;
        changed.push_back(caller);
      }
    }
  }
}

void RetainCallDependencies(const CodeReuseCache& cache,
                            const std::vector<std::vector<std::string>>& symbols,
                            std::size_t index,
                            absl::flat_hash_set<const void*>& retained,
                            CodeReusePlan& plan) {
  for (const auto callee : plan.inputs.at(index).dependencies) {
    if (plan.reused.at(callee) == 0) {
      continue;
    }
    for (const auto& symbol : symbols.at(callee)) {
      const auto& cached = cache.functions.at(symbol);
      auto resource = cached.resource.lock();
      if (retained.insert(resource.get()).second) {
        plan.dependencies.push_back(std::move(resource));
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
    const auto& input = plan.inputs.at(index);
    const auto* source = index < cache.sources.size() ? &cache.sources.at(index) : nullptr;
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
    plan.reused.at(index) = reusable ? 1 : 0;
    if (!reusable) {
      changed.push_back(index);
    }
    for (const auto dependency : input.dependencies) {
      callers.at(dependency).push_back(index);
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
  absl::flat_hash_set<const void*> retained;
  for (std::size_t index = 0; index < symbols.size(); ++index) {
    const auto reused = plan.reused.at(index) != 0;
    if (!reused) {
      ++plan.compiled_functions;
      RetainCallDependencies(cache, symbols, index, retained, plan);
    }
    for (const auto& name : symbols.at(index)) {
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
