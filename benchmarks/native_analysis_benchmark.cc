#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"
#include "compiler/native/borrow_analysis.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/field_access.h"
#include "compiler/native/recursive_inliner.h"

namespace vela::benchmarks {
namespace {

void RepeatedLeafPlanning(benchmark::State& state) {
  constexpr std::size_t kLeafBodyInstructions = 96;
  ir::Program program;
  program.native_mode = NativeProgramMode::Reloadable;
  const auto callers = static_cast<std::size_t>(state.range(1));
  program.functions.resize(callers + 1);
  auto& callee = program.functions.back();
  callee.return_type.kind = frontend::TypeDescriptor::Kind::Int;
  callee.register_count = 1;
  callee.value_types.push_back(callee.return_type);
  for (std::size_t index = 0; index < kLeafBodyInstructions; ++index) {
    callee.instructions.push_back({.opcode = Opcode::LoadInt, .location = {}});
  }
  callee.instructions.push_back({.opcode = Opcode::Return, .location = {}});
  for (std::size_t index = 0; index < callers; ++index) {
    auto& caller = program.functions[index];
    caller.return_type = callee.return_type;
    caller.register_count = 1;
    caller.value_types.push_back(callee.return_type);
    for (std::int64_t call = 0; call < state.range(0); ++call) {
      caller.instructions.push_back(
          {.opcode = Opcode::Call, .index = static_cast<std::uint32_t>(callers), .location = {}});
    }
    caller.instructions.push_back({.opcode = Opcode::Jump, .index = 0, .location = {}});
  }
  for (auto iteration : state) {
    auto plan = compiler::native::PlanNativeCalls(program, NativeCodeTier::Optimized);
    benchmark::DoNotOptimize(plan);
    for (std::size_t index = 0; index < callers; ++index) {
      if (!plan.slot_calls[index].empty() || plan.dependencies[index].size() != 1 ||
          plan.dependencies[index].front() != callers) {
        state.SkipWithError("Repeated leaf lost its direct dependency");
        return;
      }
    }
  }
}

enum class IneligibleRecursion : std::uint8_t { Nonrecursive, OverBudget };

enum class AnalysisPass : std::uint8_t { Escapes, Borrows, Generation };

void ObjectAnalysis(benchmark::State& state, AnalysisPass pass) {
  const auto count = state.range(0);
  std::string updates;
  for (std::int64_t index = 0; index < count; ++index) {
    updates += "box.value = box.value + value;";
  }
  const auto source =
      "class Box { value: int; add(value: int): int { const box = this;" + updates +
      "return box.value; } } func kernel(value: int): int { const box = Box(value: value);" +
      updates + "return box.value; } export func main(): int { return kernel(1); }";
  const frontend::SourceInput input{.path = "analysis.vela", .text = source};
  const auto prepared = ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
  if (!prepared.Succeeded()) {
    state.SkipWithError(frontend::DiagnosticsText(prepared.diagnostics));
    return;
  }
  const auto& program = prepared.program;
  const auto found = std::ranges::find(program.functions, "kernel", &ir::Function::name);
  const auto function = static_cast<std::size_t>(found - program.functions.begin());
  const auto calls = compiler::native::PlanNativeCalls(program, NativeCodeTier::Baseline);
  const auto fields = compiler::native::FindFieldTargets(program);
  for (auto iteration : state) {
    if (pass == AnalysisPass::Escapes) {
      auto plan = compiler::native::PlanEscapes(program, function);
      if (plan.allocations.empty() || !plan.allocations.front().can_scalar_replace) {
        state.SkipWithError("Object did not undergo scalar replacement");
        return;
      }
      benchmark::DoNotOptimize(plan);
    } else if (pass == AnalysisPass::Borrows) {
      auto plans = compiler::native::PlanBorrows(program);
      if (std::ranges::none_of(plans, [](const auto& plan) { return plan.has_value(); })) {
        state.SkipWithError("Object method did not acquire a borrowed entry");
        return;
      }
      benchmark::DoNotOptimize(plans);
    } else {
      auto generated = compiler::native::Generate(program, calls, fields, "analysis");
      if (!generated) {
        state.SkipWithError(frontend::DiagnosticsText(generated.error()));
        return;
      }
      if (std::ranges::none_of(*generated->module, [](const llvm::Function& body) {
            return body.getName().ends_with("_range");
          })) {
        state.SkipWithError("Object kernel did not acquire a range specialization");
        return;
      }
      benchmark::DoNotOptimize(generated);
    }
  }
}

void MixedComponentPlanning(benchmark::State& state) {
  constexpr std::size_t kComponentFunctions = 32;
  constexpr std::size_t kScalarInstructions = 48;
  ir::Program program;
  program.native_mode = NativeProgramMode::Reloadable;
  program.functions.resize(kComponentFunctions + 1);
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    auto& function = program.functions[index];
    function.return_type.kind = frontend::TypeDescriptor::Kind::Int;
    function.register_count = 1;
    function.value_types.push_back(function.return_type);
    if (index < kComponentFunctions) {
      function.is_generated_constructor = true;
      for (std::size_t instruction = 0; instruction < kScalarInstructions; ++instruction) {
        function.instructions.push_back({.opcode = Opcode::LoadInt, .location = {}});
      }
      function.instructions.push_back({
          .opcode = Opcode::Call,
          .index = static_cast<std::uint32_t>((index + 1) % kComponentFunctions),
          .location = {},
      });
      function.instructions.push_back(
          {.opcode = Opcode::Call, .index = kComponentFunctions, .location = {}});
    }
    function.instructions.push_back({.opcode = Opcode::Return, .location = {}});
  }
  for (auto iteration : state) {
    auto plan = compiler::native::PlanNativeCalls(program, NativeCodeTier::Optimized);
    benchmark::DoNotOptimize(plan);
    for (std::size_t index = 0; index < kComponentFunctions; ++index) {
      const auto& slots = plan.slot_calls[index];
      if (slots.size() != 1 || slots.front() != kComponentFunctions) {
        state.SkipWithError("Component edge escaped through an entry slot");
        return;
      }
    }
  }
}

void IneligibleRecursiveAnalysis(benchmark::State& state, IneligibleRecursion scenario) {
  llvm::LLVMContext context;
  llvm::Module module("ineligible", context);
  llvm::IRBuilder<> builder(context);
  auto* type = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt1Ty()}, false);
  auto* function = llvm::Function::Create(type, llvm::Function::InternalLinkage, "body", module);
  auto* block = llvm::BasicBlock::Create(context, "entry", function);
  const auto count = compiler::native::kMaximumRecursiveInlineInstructions;
  for (std::size_t index = 0; index < count; ++index) {
    builder.SetInsertPoint(block);
    auto* next = llvm::BasicBlock::Create(context, "next", function);
    builder.CreateBr(next);
    block = next;
  }
  builder.SetInsertPoint(block);
  auto* done = llvm::BasicBlock::Create(context, "done", function);
  builder.CreateCondBr(function->getArg(0), function->getEntryBlock().getNextNode(), done);
  builder.SetInsertPoint(done);
  if (scenario == IneligibleRecursion::OverBudget) {
    builder.CreateCall(function, {builder.getFalse()});
  }
  builder.CreateRet(builder.getInt64(0));
  if (llvm::verifyModule(module)) {
    state.SkipWithError("Invalid recursive analysis input");
    return;
  }
  const auto instructions = function->getInstructionCount();
  for (auto iteration : state) {
    compiler::native::InlineRecursion(*function);
    benchmark::DoNotOptimize(function);
  }
  if (function->getInstructionCount() != instructions || module.size() != 1) {
    state.SkipWithError("Ineligible body was expanded");
  }
}

BENCHMARK(RepeatedLeafPlanning)
    ->Name("compile/native/repeated_leaf")
    ->Args({1, 1})
    ->Args({256, 1})
    ->Args({1, 128});
BENCHMARK_CAPTURE(ObjectAnalysis, Escapes, AnalysisPass::Escapes)
    ->Name("compile/native/escape_rewrite")
    ->Arg(256);
BENCHMARK_CAPTURE(ObjectAnalysis, Borrows, AnalysisPass::Borrows)
    ->Name("compile/native/borrow_rewrite")
    ->Arg(256);
BENCHMARK_CAPTURE(ObjectAnalysis, Generation, AnalysisPass::Generation)
    ->Name("compile/native/range_generation")
    ->Arg(256);
BENCHMARK(MixedComponentPlanning)->Name("compile/native/mixed_component");
BENCHMARK_CAPTURE(IneligibleRecursiveAnalysis, Nonrecursive, IneligibleRecursion::Nonrecursive)
    ->Name("compile/native/nonrecursive");
BENCHMARK_CAPTURE(IneligibleRecursiveAnalysis, OverBudget, IneligibleRecursion::OverBudget)
    ->Name("compile/native/recursive_over_budget");

}  // namespace
}  // namespace vela::benchmarks
