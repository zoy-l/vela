#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "base/binary.h"
#include "base/declaration.h"
#include "compiler/development/development_changes.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/ir/ir.h"
#include "compiler/jit/code_inputs.h"
#include "compiler/jit/function_compiler.h"
#include "compiler/native/function_symbols.h"

namespace vela::compiler::jit {

enum class CompilerMessage : std::uint8_t { Response, FilesChanged };

enum class CompilerCommand : std::uint8_t {
  Prepare,
  Compile,
  Release,
  CreateSources,
  PrepareSources,
  ReleaseSources,
  ReleaseCandidate,
  PrepareSourceCode,
  CreateFiles,
  StartWatching,
  CaptureFiles,
  AcceptFiles,
  PollFiles,
  WatchFiles,
  ReleaseFiles,
};

BinaryWriter CompilerRequest(CompilerCommand command, std::uint64_t resource_id);

struct SourcePreparationRequest final {
  std::uint64_t committed = 0;
  SourceUpdate update;
  std::vector<DeclarationId> retained;
};

struct PreparedSourceDescription final {
  std::uint64_t token = 0;
  std::uint64_t revision = 0;
  std::vector<std::string> changed_sources;
  std::vector<SourceChange> sources;
  std::vector<std::string> roots;
  std::vector<std::string> standard_names;
  DevelopmentChanges changes;
  frontend::Diagnostics diagnostics;
  std::vector<frontend::ReflectionSelection> reflection;
  std::vector<frontend::DeclarationRecord> identities;
  std::uint64_t loading = 0, checking = 0, ir = 0;
};

using SourcePreparationResponse = std::variant<SourceCandidateError, PreparedSourceDescription>;
struct FileOperationResponse final {
  frontend::Diagnostics diagnostics;
  std::optional<SourceUpdate> update;
  std::string entry;
};
void EncodeFileResponse(BinaryWriter& writer, const FileOperationResponse& response);
std::expected<FileOperationResponse, std::string> DecodeFileResponse(
    std::span<const std::byte> bytes);
std::vector<std::byte> EncodeSourceUpdate(const std::optional<SourceUpdate>& update);
std::expected<std::optional<SourceUpdate>, std::string> DecodeSourceUpdate(
    std::span<const std::byte> bytes);
std::vector<std::byte> EncodeSourceConfiguration(const SourceSessionConfiguration& configuration);
std::expected<SourceSessionConfiguration, std::string> DecodeSourceConfiguration(
    std::span<const std::byte> bytes);
std::vector<std::byte> EncodeSourceRequest(const SourcePreparationRequest& request);
std::expected<SourcePreparationRequest, std::string> DecodeSourceRequest(
    std::span<const std::byte> bytes);
void EncodeSourceResponse(BinaryWriter& writer, const SourcePreparationResponse& response);
std::expected<SourcePreparationResponse, std::string> DecodeSourceResponse(
    std::span<const std::byte> bytes);
struct FunctionCompilationDescription final {
  std::vector<native::FunctionEntrySymbols> symbols;
  std::vector<FunctionCodeInputs> inputs;
  std::vector<std::byte> image;
};

// 帧与 Typed IR 都不携带原生指针、堆对象或机器地址。双端来自同一 SDK 构建。
std::vector<std::byte> EncodeProgram(const ir::Program& program);
std::vector<std::byte> EncodeFunction(const ir::Function& function);
std::vector<std::byte> EncodeCompilationEnvironment(const ir::Program& program);
std::expected<ir::Program, std::string> DecodeProgram(std::span<const std::byte> bytes);
void EncodeFunctions(BinaryWriter& writer,
                     const FunctionCompiler& compiler,
                     const std::vector<std::byte>& image);
std::expected<FunctionCompilationDescription, std::string> DecodeFunctions(
    std::span<const std::byte> bytes);
std::expected<std::vector<std::byte>, std::string> ReadCompilerFrame(std::FILE* input);
std::expected<void, std::string> WriteCompilerFrame(std::FILE* output,
                                                    std::span<const std::byte> bytes);

int RunCompilerService();

}  // namespace vela::compiler::jit
