#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/jit/code_inputs.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/function_compiler.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include "base/binary.h"
#include "base/fingerprint.h"
#include "compiler/cache_build_identity.h"
#include "compiler/jit/code_cache.h"
#include "compiler/jit/compiler_protocol.h"
#include "compiler/jit/engine.h"
#include "compiler/jit/lazy_compiler.h"
#include "compiler/jit/object_compilation.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/native_image_data.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::jit {
namespace {

class LocalFunctionCompiler final : public FunctionCompiler {
 public:
  LocalFunctionCompiler(std::shared_ptr<const ir::Program> program,
                        native::BaselineCompilationPlan plan,
                        CompileOptions options)
      : program_(std::move(program)),
        plan_(std::move(plan)),
        options_(std::move(options)),
        bodies_(program_->functions.size()) {
    symbols = std::move(plan_.symbols);
    for (const auto& entry : symbols) {
      names_.insert(entry.implementations.begin(), entry.implementations.end());
    }
    inputs = DescribeCodeInputs(*program_, plan_.calls, plan_.fields, options_.tier,
                                static_cast<std::uint8_t>(options_.optimization),
                                static_cast<std::uint8_t>(options_.debugging),
                                static_cast<std::uint8_t>(options_.escape));
  }

  std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::string> Compile(
      std::size_t function) override {
    const auto key = options_.code_cache ? CacheKey(function) : std::string{};
    auto compiled = CompileCachedObject(
        options_.code_cache, key,
        [&] -> std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::string> {
          auto generated = native::GenerateBaselineFunction(*program_, plan_, function);
          if (!generated) {
            return std::unexpected(frontend::DiagnosticsText(generated.error()));
          }
          for (auto& entry : *generated->module) {
            if (!names_.contains(entry.getName().str())) {
              continue;
            }
            // 每个版本拥有独立 JITDylib，目标文件无需烘入进程地址或发布批次号。
            if (entry.isDeclaration()) {
              entry.setName(entry.getName() + llvm::StringRef(kFunctionEntrySuffix));
            }
            entry.setLinkage(llvm::GlobalValue::ExternalLinkage);
          }
          if (const auto finalized = native::Finalize(*generated); !finalized) {
            return std::unexpected(finalized.error());
          }
          return native::EmitObject(*generated);
        });
    if (!compiled) {
      return std::unexpected(std::move(compiled.error()));
    }
    return std::move(compiled->object);
  }

 private:
  std::string CacheKey(std::size_t function) {
    std::call_once(environment_ready_, [&] {
      BinaryWriter identity;
      identity.Write("Vela baseline function");
      identity.Write(kCompilerBuildIdentity);
      identity.Write(LLVM_VERSION_STRING);
      identity.Write(runtime::NativeRuntimeAbiIdentity());
      identity.Write(runtime::NativeTargetIdentity());
      identity.Write(plan_.cpu.name);
      identity.Write(plan_.cpu.features);
      identity.Write(plan_.data_layout);
      const auto environment = ContentFingerprint(EncodeCompilationEnvironment(*program_));
      identity.Write(environment.low);
      identity.Write(environment.high);
      identity.Write(static_cast<std::uint8_t>(options_.optimization));
      identity.Write(static_cast<std::uint8_t>(options_.debugging));
      identity.Write(static_cast<std::uint8_t>(options_.tier));
      identity.Write(static_cast<std::uint8_t>(options_.escape));
      environment_ = ContentFingerprint(identity.bytes());
    });

    BinaryWriter identity;
    identity.Write(environment_.low);
    identity.Write(environment_.high);
    identity.Write(static_cast<std::uint64_t>(function));
    const auto body = BodyFingerprint(function);
    identity.Write(body.low);
    identity.Write(body.high);
    for (const auto callee : plan_.needed[function]) {
      if (callee == function) {
        continue;
      }
      identity.Write(static_cast<std::uint64_t>(callee));
      const auto callee_body = BodyFingerprint(callee);
      identity.Write(callee_body.low);
      identity.Write(callee_body.high);
    }
    return ContentFingerprint(identity.bytes()).Hex();
  }

  Fingerprint BodyFingerprint(std::size_t index) {
    auto& body = bodies_[index];
    std::call_once(body.ready, [&] {
      body.fingerprint = ContentFingerprint(EncodeFunction(program_->functions[index]));
    });
    return body.fingerprint;
  }

  std::shared_ptr<const ir::Program> program_;
  native::BaselineCompilationPlan plan_;
  CompileOptions options_;
  absl::flat_hash_set<std::string> names_;
  struct Body final {
    std::once_flag ready;
    Fingerprint fingerprint;
  };
  std::once_flag environment_ready_;
  Fingerprint environment_;
  std::vector<Body> bodies_;
};

}  // namespace

std::expected<std::shared_ptr<FunctionCompiler>, frontend::Diagnostics> PrepareFunctions(
    std::shared_ptr<const ir::Program> program,
    const CompileOptions& options) {
  if (program->native_mode != NativeProgramMode::Reloadable ||
      options.tier != NativeCodeTier::Baseline || options.preserve_ir) {
    return std::unexpected(CompilationDiagnostic(
        "Function compilation requires reloadable baseline code without a whole-program IR dump"));
  }
  auto plan = native::PlanBaselineCompilation(*program, options.debugging, options.escape,
                                              options.optimization);
  if (!plan) {
    return std::unexpected(std::move(plan.error()));
  }
  return std::make_shared<LocalFunctionCompiler>(std::move(program), std::move(*plan), options);
}

std::expected<CompiledImage, frontend::Diagnostics> CompileOnDemand(
    const std::shared_ptr<CodeLinker>& jit,
    const std::shared_ptr<CodeResourceCounters>& counters,
    std::uint64_t batch,
    const std::shared_ptr<const ir::Program>& program,
    const CompileOptions& options,
    CodeReuseCache* reuse) {
  auto functions = PrepareFunctions(program, options);
  if (!functions) {
    return std::unexpected(std::move(functions.error()));
  }
  return LinkFunctions(jit, counters, batch, native::DescribeNativeImage(*program), options, reuse,
                       std::move(*functions));
}

}  // namespace vela::compiler::jit
