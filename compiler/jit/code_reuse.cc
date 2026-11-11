#include "compiler/jit/code_reuse.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
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

std::optional<std::vector<ReuseFunction>> ResolveReusableCode(const CodeReuseCache::Source& source,
                                                              const CodeReuseCache& cache) {
  std::vector<ReuseFunction> entries;
  entries.reserve(source.symbols.size());
  for (const auto& name : source.symbols) {
    const auto found = cache.functions.find(name);
    if (found == cache.functions.end()) {
      return std::nullopt;
    }
    const auto& cached = found->second;
    auto resource = cached.resource.lock();
    if (!resource) {
      return std::nullopt;
    }
    entries.push_back({
        .name = name,
        .symbol = cached.symbol,
        .resource = std::move(resource),
        .address = cached.address,
    });
  }
  return entries;
}

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
  plan.retained.resize(count);
  std::vector<std::vector<std::size_t>> callers(count);
  std::vector<std::size_t> changed;
  for (std::size_t index = 0; index < count; ++index) {
    const auto& input = plan.inputs[index];
    const auto* source = index < cache.sources.size() ? &cache.sources[index] : nullptr;
    auto retained = source != nullptr && source->fingerprint == input.fingerprint
                        ? ResolveReusableCode(*source, cache)
                        : std::nullopt;
    const auto reusable = retained.has_value();
    if (retained) {
      plan.retained[index] = std::move(*retained);
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
  for (std::size_t index = 0; index < count; ++index) {
    if (plan.reused[index] == 0) {
      plan.retained[index].clear();
    }
  }
  return plan;
}

void BindReusableCode(llvm::Module& module,
                      const std::vector<std::vector<std::string>>& symbols,
                      std::uint64_t batch,
                      CodeReusePlan& plan) {
  auto retained = std::move(plan.retained);
  const auto suffix = "_batch_" + std::to_string(batch);
  for (std::size_t index = 0; index < symbols.size(); ++index) {
    const auto reused = plan.reused[index] != 0;
    const auto first = plan.functions.size();
    if (reused) {
      auto& entries = retained[index];
      plan.functions.insert(plan.functions.end(), std::make_move_iterator(entries.begin()),
                            std::make_move_iterator(entries.end()));
    } else {
      ++plan.compiled_functions;
      for (const auto& name : symbols[index]) {
        plan.functions.push_back({
            .name = name,
            .symbol = name + suffix,
            .resource = {},
        });
      }
    }
    for (auto& entry : std::span(plan.functions).subspan(first)) {
      if (auto* function = module.getFunction(entry.name)) {
        if (reused) {
          function->removeFnAttr(llvm::Attribute::AlwaysInline);
        }
        function->setName(entry.symbol);
        function->setLinkage(llvm::GlobalValue::ExternalLinkage);
      }
    }
  }
}

}  // namespace vela::compiler::jit
