#include <llvm/ADT/StringRef.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/TargetParser/Triple.h>
#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/ir/ir.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/jit/compiler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "base/binary.h"
#include "base/path.h"
#include "compiler/cache_build_identity.h"
#include "compiler/jit/compiler_process.h"
#include "compiler/jit/compiler_protocol.h"
#include "compiler/jit/engine.h"
#include "compiler/jit/external_sources.h"
#include "compiler/jit/function_compiler.h"
#include "compiler/jit/object_buffer.h"
#include "compiler/native/native_image_data.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::jit {
namespace {

class ExternalFunctions final : public FunctionCompiler {
 public:
  ExternalFunctions(std::shared_ptr<CompilerProcess> process,
                    std::uint64_t resource_id,
                    FunctionCompilationDescription description)
      : process_(std::move(process)), id_(resource_id) {
    symbols = std::move(description.symbols);
    inputs = std::move(description.inputs);
  }
  ExternalFunctions(const ExternalFunctions&) = delete;
  ExternalFunctions& operator=(const ExternalFunctions&) = delete;
  ExternalFunctions(ExternalFunctions&&) = delete;
  ExternalFunctions& operator=(ExternalFunctions&&) = delete;
  ~ExternalFunctions() override { process_->Release(CompilerCommand::Release, id_); }
  std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::string> Compile(
      std::size_t function) override {
    auto request = CompilerRequest(CompilerCommand::Compile, id_);
    request.Write(static_cast<std::uint64_t>(function));
    auto bytes = process_->Exchange(request.bytes());
    if (!bytes) {
      return std::unexpected(std::move(bytes.error()));
    }
    return OwnObjectBytes(std::move(*bytes));
  }

 private:
  std::shared_ptr<CompilerProcess> process_;
  std::uint64_t id_;
};

class ExternalCompiler final : public Compiler {
 public:
  ExternalCompiler(std::shared_ptr<CompilerProcess> process,
                   std::shared_ptr<CodeLinker> linker,
                   std::filesystem::path cache)
      : Compiler(std::move(linker)), process_(std::move(process)), cache_(std::move(cache)) {}
  std::expected<CompiledImage, frontend::Diagnostics> Compile(const ir::Program& program,
                                                              const CompileOptions& options,
                                                              CodeReuseCache* reuse) override {
    return CompilePrepared(&program, options, reuse, 0);
  }
  std::expected<CompiledImage, frontend::Diagnostics> Compile(
      std::shared_ptr<const ir::Program> program,
      const CompileOptions& options,
      CodeReuseCache* reuse) override {
    return CompilePrepared(program.get(), options, reuse, 0);
  }
  std::expected<CompiledImage, frontend::Diagnostics> CompileSource(
      const std::shared_ptr<const CompiledSourceSnapshot>& source,
      const CompileOptions& options,
      CodeReuseCache* reuse) override {
    const auto token = source->compiler_token;
    return CompilePrepared(nullptr, options, reuse, token);
  }
  std::expected<std::shared_ptr<SourceSession>, frontend::Diagnostics> CreateSources(
      const SourceSessionConfiguration& configuration) override {
    return CreateExternalSources(process_, next_program_.fetch_add(1), configuration);
  }
  std::expected<std::unique_ptr<DevelopmentFiles>, frontend::Diagnostics> CreateFiles(
      const std::string& entry,
      const frontend::ModulePaths& imports) override {
    return CreateExternalFiles(process_, next_program_.fetch_add(1), entry, imports);
  }

 private:
  std::expected<CompiledImage, frontend::Diagnostics> CompilePrepared(const ir::Program* program,
                                                                      const CompileOptions& options,
                                                                      CodeReuseCache* reuse,
                                                                      std::uint64_t token) {
    if (options.tier != NativeCodeTier::Baseline || options.preserve_ir) {
      return std::unexpected(
          CompilationDiagnostic("External compilation requires reloadable baseline code"));
    }
    const auto started = std::chrono::steady_clock::now();
    const auto resource_id = next_program_.fetch_add(1, std::memory_order_relaxed);
    auto request = CompilerRequest(
        token == 0 ? CompilerCommand::Prepare : CompilerCommand::PrepareSourceCode, resource_id);
    request.Write(static_cast<std::uint8_t>(options.optimization));
    request.Write(static_cast<std::uint8_t>(options.debugging));
    request.Write(static_cast<std::uint8_t>(options.tier));
    request.Write(static_cast<std::uint8_t>(options.escape));
    request.Write(PathText(options.code_cache.value_or(cache_)));
    if (token == 0) {
      EncodeProgram(request, *program);
    } else {
      request.Write(token);
    }
    auto response = process_->Exchange(request.bytes());
    if (!response) {
      return std::unexpected(CompilationDiagnostic(std::move(response.error())));
    }
    auto description = DecodeFunctions(*response);
    if (!description) {
      return std::unexpected(CompilationDiagnostic(std::move(description.error())));
    }
    auto data = (program != nullptr)
                    ? std::expected<std::shared_ptr<native::NativeImageData>, std::string>(
                          native::DescribeNativeImage(*program))
                    : native::DecodeNativeImage(description->image);
    if (!data) {
      return std::unexpected(CompilationDiagnostic(std::move(data.error())));
    }
    const auto count = (*data)->image()->functions.size;
    if (description->symbols.size() != count || description->inputs.size() != count) {
      return std::unexpected(
          CompilationDiagnostic("compiler returned an inconsistent function table"));
    }
    auto functions =
        std::make_shared<ExternalFunctions>(process_, resource_id, std::move(*description));
    auto result = LinkFunctions(linker(), resource_counters(), resource_id, *data, options, reuse,
                                std::move(functions));
    if (result) {
      result->timings.generation = std::chrono::steady_clock::now() - started;
    }
    return result;
  }

  std::shared_ptr<CompilerProcess> process_;
  std::filesystem::path cache_;
  std::atomic_uint64_t next_program_{0};
};

}  // namespace

std::expected<std::unique_ptr<Compiler>, frontend::Diagnostics> Compiler::CreateExternal(
    CompilerProcessOptions options) {
  auto process = CompilerProcess::Start(options.executable);
  if (!process) {
    return std::unexpected(CompilationDiagnostic(std::move(process.error())));
  }
  auto hello = (*process)->Receive();
  if (!hello) {
    return std::unexpected(CompilationDiagnostic(std::move(hello.error())));
  }
  BinaryReader reader(*hello);
  const auto build = reader.Text();
  const auto abi = reader.Text();
  const auto target = reader.Text();
  const auto layout = reader.Text();
  if (!build || !abi || !target || !layout || reader.remaining() != 0 ||
      *build != kCompilerBuildIdentity || *abi != runtime::NativeRuntimeAbiIdentity() ||
      *target != runtime::NativeTargetIdentity()) {
    return std::unexpected(CompilationDiagnostic(
        "compiler process and runtime must use the same SDK build and target"));
  }
  auto data_layout = llvm::DataLayout::parse(*layout);
  if (!data_layout) {
    return std::unexpected(CompilationDiagnostic(llvm::toString(data_layout.takeError())));
  }
  auto linker = CreateEngine(llvm::Triple(*target), *data_layout);
  if (!linker) {
    return std::unexpected(CompilationDiagnostic(llvm::toString(linker.takeError())));
  }
  (*process)->StartReceiving();
  return std::make_unique<ExternalCompiler>(std::move(*process), std::move(*linker),
                                            std::move(options.cache));
}

}  // namespace vela::compiler::jit
