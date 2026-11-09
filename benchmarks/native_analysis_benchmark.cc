#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"
#include "compiler/native/call_targets.h"
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
BENCHMARK(MixedComponentPlanning)->Name("compile/native/mixed_component");
BENCHMARK_CAPTURE(IneligibleRecursiveAnalysis, Nonrecursive, IneligibleRecursion::Nonrecursive)
    ->Name("compile/native/nonrecursive");
BENCHMARK_CAPTURE(IneligibleRecursiveAnalysis, OverBudget, IneligibleRecursion::OverBudget)
    ->Name("compile/native/recursive_over_budget");

}  // namespace
}  // namespace vela::benchmarks
