#include "base/native_program_mode.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_progress.h"
#include "compiler/ir/ir.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/compiler_protocol.h"
#include "compiler/jit/function_compiler.h"
#include "compiler/native/compile_options.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/native_roots.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/binary.h"
#include "base/path.h"
#include "compiler/cache_build_identity.h"
#include "compiler/development/development_files.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/native_image_data.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::jit {
namespace {

class ServiceOutput final {
 public:
  std::expected<void, std::string> Send(std::span<const std::byte> message) {
    const std::scoped_lock lock(mutex_);
    return WriteCompilerFrame(stdout, message);
  }
  void FilesChanged(std::uint64_t resource_id) {
    BinaryWriter message;
    message.Write(resource_id);
    message.Write(static_cast<std::uint8_t>(CompilerMessage::FilesChanged));
    if (const auto sent = Send(message.bytes()); !sent) {
      std::println(stderr, "Vela file notification: {}", sent.error());
    }
  }
  void Progress(const frontend::SourceProgress& progress) {
    BinaryWriter message;
    message.Write(static_cast<std::uint8_t>(progress.kind));
    message.Write(progress.path);
    message.Write(static_cast<std::uint64_t>(progress.completed));
    message.Write(static_cast<std::uint64_t>(progress.total));
    message.Write(static_cast<std::uint8_t>(CompilerMessage::SourceProgress));
    if (const auto sent = Send(message.bytes()); !sent) {
      std::println(stderr, "Vela compiler progress: {}", sent.error());
    }
  }

 private:
  std::mutex mutex_;
};

struct CandidateRecord final {
  std::uint64_t source;
  SourceCandidate candidate;
};
struct ServiceState final {
  ServiceOutput* output = nullptr;
  absl::flat_hash_map<std::uint64_t, std::shared_ptr<FunctionCompiler>> programs;
  absl::flat_hash_map<std::uint64_t, std::shared_ptr<SourceSession>> sources;
  absl::flat_hash_map<std::uint64_t, std::unique_ptr<DevelopmentFiles>> files;
  absl::flat_hash_map<std::uint64_t, CandidateRecord> candidates;
  std::uint64_t next_candidate = 1;
};

std::expected<void, std::string> PrepareCode(BinaryReader& reader,
                                             ServiceState& state,
                                             BinaryWriter& output,
                                             CompilerCommand command,
                                             std::uint64_t resource_id) {
  const auto modes = reader.Record<std::uint8_t, std::uint8_t, std::uint8_t, std::uint8_t>();
  const auto cache = reader.Text();
  if (!modes || !cache) {
    return std::unexpected("invalid compiler preparation options");
  }
  const auto [optimization, debugging, tier, escape] = *modes;
  if (optimization > static_cast<std::uint8_t>(NativeOptimization::Release) ||
      debugging > static_cast<std::uint8_t>(native::NativeDebugging::Instructions) ||
      tier != static_cast<std::uint8_t>(NativeCodeTier::Baseline) ||
      escape > static_cast<std::uint8_t>(native::EscapeOptimization::Enabled)) {
    return std::unexpected("unsupported compiler preparation options");
  }
  std::shared_ptr<const ir::Program> program;
  if (command == CompilerCommand::PrepareSourceCode) {
    const auto token = reader.Read<std::uint64_t>();
    if (!token || reader.remaining() != 0) {
      return std::unexpected("invalid source compilation candidate");
    }
    const auto candidate = state.candidates.find(*token);
    if (candidate == state.candidates.end()) {
      return std::unexpected("unknown source compilation candidate");
    }
    // 机器码版本只保活 Typed IR，不固定解析树和旧前端缓存。
    program = candidate->second.candidate.snapshot().program;
  } else {
    auto payload = reader.Take(reader.remaining());
    auto decoded = DecodeProgram(*payload);
    if (!decoded) {
      return std::unexpected(std::move(decoded.error()));
    }
    program = std::make_shared<const ir::Program>(std::move(*decoded));
  }
  CompileOptions options;
  options.optimization = static_cast<NativeOptimization>(optimization);
  options.debugging = static_cast<native::NativeDebugging>(debugging);
  options.tier = static_cast<NativeCodeTier>(tier);
  options.escape = static_cast<native::EscapeOptimization>(escape);
  options.materialization = Materialization::OnDemand;
  if (!cache->empty()) {
    options.code_cache = Utf8Path(*cache);
  }
  std::vector<std::byte> image;
  if (command == CompilerCommand::PrepareSourceCode) {
    image = native::EncodeNativeImage(*native::DescribeNativeImage(*program)->image());
  }
  auto compiler = PrepareFunctions(std::move(program), options);
  if (!compiler) {
    return std::unexpected(frontend::DiagnosticsText(compiler.error()));
  }
  EncodeFunctions(output, **compiler, image);
  if (!state.programs.try_emplace(resource_id, std::move(*compiler)).second) {
    return std::unexpected("duplicate compiler program");
  }
  return {};
}

std::expected<void, std::string> CompileFunction(BinaryReader& reader,
                                                 ServiceState& state,
                                                 BinaryWriter& output,
                                                 std::uint64_t resource_id) {
  const auto function = reader.Read<std::uint64_t>();
  const auto found = state.programs.find(resource_id);
  if (!function || reader.remaining() != 0 || found == state.programs.end() ||
      *function >= found->second->symbols.size()) {
    return std::unexpected("invalid compiler function request");
  }
  auto object = found->second->Compile(static_cast<std::size_t>(*function));
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }
  const auto buffer = (*object)->getBuffer();
  const auto payload = std::as_bytes(std::span(buffer.data(), buffer.size()));
  output.Write(payload);
  return {};
}

std::expected<void, std::string> PrepareSources(BinaryReader& reader,
                                                ServiceState& state,
                                                BinaryWriter& output,
                                                std::uint64_t resource_id) {
  const auto payload = reader.Take(reader.remaining());
  auto request = DecodeSourceRequest(*payload);
  if (!request) {
    return std::unexpected(std::move(request.error()));
  }
  const auto found = state.sources.find(resource_id);
  if (found == state.sources.end()) {
    return std::unexpected("invalid source preparation");
  }
  auto& source = *found->second;
  if (request->committed != 0) {
    const auto committed = state.candidates.find(request->committed);
    if (committed == state.candidates.end() || committed->second.source != resource_id) {
      return std::unexpected("unknown source baseline");
    }
    const auto& candidate = committed->second.candidate;
    if (source.snapshot() != candidate.snapshot_owner() && !source.Commit(candidate)) {
      EncodeSourceResponse(output, SourceCandidateError{
                                       .status = SourceCandidateStatus::Stale,
                                       .diagnostics = {},
                                       .declaration = {},
                                   });
      return {};
    }
  }
  frontend::SourceProgressCallback progress;
  if (request->report_progress) {
    progress = [messages = state.output](const frontend::SourceProgress& event) {
      messages->Progress(event);
    };
  }
  auto prepared = source.Prepare(std::move(request->update), request->retained, progress);
  if (!prepared) {
    EncodeSourceResponse(output, std::move(prepared.error()));
    return {};
  }
  const auto token = state.next_candidate++;
  const auto& snapshot = prepared->snapshot();
  PreparedSourceDescription description;
  description.token = token;
  description.revision = snapshot.revision;
  description.changed_sources = snapshot.changed_sources;
  for (const auto& path : snapshot.changed_sources) {
    const auto source_text = snapshot.sources.find(path);
    description.sources.push_back({
        .path = path,
        .text = source_text == snapshot.sources.end()
                    ? std::nullopt
                    : std::optional<std::string>(*source_text->second),
    });
  }
  description.roots = snapshot.roots;
  description.standard_names = snapshot.standard_names;
  description.changes = snapshot.changes;
  description.diagnostics = snapshot.diagnostics;
  description.reflection = snapshot.reflection;
  const auto identities = snapshot.identities.records();
  description.identities.assign(identities.begin(), identities.end());
  description.loading = static_cast<std::uint64_t>(snapshot.timings.loading.count());
  description.checking = static_cast<std::uint64_t>(snapshot.timings.checking.count());
  description.ir = static_cast<std::uint64_t>(snapshot.timings.ir.count());
  state.candidates.try_emplace(
      token, CandidateRecord{.source = resource_id, .candidate = std::move(*prepared)});
  EncodeSourceResponse(output, std::move(description));
  return {};
}

std::expected<void, std::string> CreateSourceSession(BinaryReader& reader,
                                                     ServiceState& state,
                                                     CompilerCommand kind,
                                                     std::uint64_t resource_id) {
  const auto payload = reader.Take(reader.remaining());
  auto configuration = DecodeSourceConfiguration(*payload);
  if (!configuration) {
    return std::unexpected(std::move(configuration.error()));
  }
  if (kind == CompilerCommand::CreateSources) {
    if (!state.sources.try_emplace(resource_id, std::make_shared<SourceSession>(*configuration))
             .second) {
      return std::unexpected("duplicate source session");
    }
  } else if (!state.files
                  .try_emplace(resource_id, DevelopmentFiles::Create(configuration->entry,
                                                                     configuration->imports))
                  .second) {
    return std::unexpected("duplicate file session");
  }
  return {};
}

std::expected<void, std::string> StartFileWatching(BinaryReader& reader,
                                                   ServiceState& state,
                                                   BinaryWriter& output,
                                                   std::uint64_t resource_id) {
  const auto notify = reader.Read<std::uint8_t>();
  const auto found = state.files.find(resource_id);
  if (!notify || *notify > 1 || reader.remaining() != 0 || found == state.files.end()) {
    return std::unexpected("invalid file watch request");
  }
  std::function<void()> changed;
  if (*notify != 0) {
    changed = [resource_id, messages = state.output] { messages->FilesChanged(resource_id); };
  }
  const auto watched = found->second->StartWatching(std::move(changed));
  EncodeFileResponse(output,
                     watched ? FileOperationResponse{.diagnostics = {},
                                                     .update = {},
                                                     .entry = found->second->entry(),}
                             : FileOperationResponse{
                                   .diagnostics = watched.error(), .update = {}, .entry = {},});
  return {};
}

std::expected<void, std::string> CaptureFileSnapshot(BinaryReader& reader,
                                                     ServiceState& state,
                                                     BinaryWriter& output,
                                                     CompilerCommand kind,
                                                     std::uint64_t resource_id) {
  const auto token = reader.Read<std::uint64_t>();
  const auto found = state.files.find(resource_id);
  if (!token || reader.remaining() != 0 || found == state.files.end()) {
    return std::unexpected("invalid file snapshot request");
  }
  std::shared_ptr<const CompiledSourceSnapshot> snapshot;
  if (*token != 0) {
    const auto candidate = state.candidates.find(*token);
    if (candidate == state.candidates.end()) {
      return std::unexpected("unknown file snapshot");
    }
    snapshot = candidate->second.candidate.snapshot_owner();
  } else {
    snapshot = std::make_shared<const CompiledSourceSnapshot>();
  }
  if (kind == CompilerCommand::AcceptFiles) {
    auto accepted = found->second->Accept(std::move(snapshot));
    EncodeFileResponse(output,
                       {
                           .diagnostics = accepted ? frontend::Diagnostics{} : accepted.error(),
                           .update = {},
                           .entry = {},
                       });
  } else {
    auto captured = found->second->Capture(*snapshot);
    EncodeFileResponse(
        output,
        captured ? FileOperationResponse{.diagnostics = {},
                                         .update = *captured,
                                         .entry = {},}
                 : FileOperationResponse{
                       .diagnostics = captured.error(), .update = {}, .entry = {},});
  }
  return {};
}

std::expected<void, std::string> PollFileSession(BinaryReader& reader,
                                                 ServiceState& state,
                                                 BinaryWriter& output,
                                                 std::uint64_t resource_id) {
  const auto timeout = reader.Read<std::uint64_t>();
  const auto found = state.files.find(resource_id);
  if (!timeout || *timeout > static_cast<std::uint64_t>(std::chrono::milliseconds::max().count()) ||
      reader.remaining() != 0 || found == state.files.end()) {
    return std::unexpected("invalid file poll request");
  }
  auto update = found->second->Poll(std::chrono::milliseconds(*timeout));
  EncodeFileResponse(
      output, update ? FileOperationResponse{.diagnostics = {}, .update = *update, .entry = {}}
                     : FileOperationResponse{
                           .diagnostics = update.error(),
                           .update = {},
                           .entry = {},
                       });
  return {};
}

std::expected<void, std::string> TrackFileSources(BinaryReader& reader,
                                                  ServiceState& state,
                                                  BinaryWriter& output,
                                                  std::uint64_t resource_id) {
  const auto payload = reader.Take(reader.remaining());
  auto decoded = DecodeSourceUpdate(*payload);
  if (!decoded) {
    return std::unexpected(std::move(decoded.error()));
  }
  auto update = std::move(*decoded);
  const auto found = state.files.find(resource_id);
  if (!update || found == state.files.end()) {
    return std::unexpected("invalid watched sources");
  }
  auto watched = found->second->WatchSources(update->changes);
  EncodeFileResponse(
      output, {
                  .diagnostics = watched ? frontend::Diagnostics{} : std::move(watched.error()),
                  .update = {},
                  .entry = {},
              });
  return {};
}

std::expected<void, std::string> HandleRequest(std::span<const std::byte> bytes,
                                               ServiceState& state,
                                               BinaryWriter& output) {
  BinaryReader reader(bytes);
  const auto header = reader.Record<std::uint8_t, std::uint64_t>();
  if (!header) {
    return std::unexpected(header.error().message);
  }
  const auto [command, resource_id] = *header;
  const auto kind = static_cast<CompilerCommand>(command);
  switch (kind) {
    case CompilerCommand::Prepare:
    case CompilerCommand::PrepareSourceCode:
      return PrepareCode(reader, state, output, kind, resource_id);
    case CompilerCommand::Compile:
      return CompileFunction(reader, state, output, resource_id);
    case CompilerCommand::Release:
      if (reader.remaining() != 0 || state.programs.erase(resource_id) != 1) {
        return std::unexpected("invalid compiler program release");
      }
      return {};
    case CompilerCommand::CreateSources:
    case CompilerCommand::CreateFiles:
      return CreateSourceSession(reader, state, kind, resource_id);
    case CompilerCommand::PrepareSources:
      return PrepareSources(reader, state, output, resource_id);
    case CompilerCommand::ReleaseSources:
      if (reader.remaining() != 0 || state.sources.erase(resource_id) != 1) {
        return std::unexpected("unknown source session release");
      }
      return {};
    case CompilerCommand::ReleaseCandidate:
      if (reader.remaining() != 0 || state.candidates.erase(resource_id) != 1) {
        return std::unexpected("unknown source candidate release");
      }
      return {};
    case CompilerCommand::StartWatching:
      return StartFileWatching(reader, state, output, resource_id);
    case CompilerCommand::CaptureFiles:
    case CompilerCommand::AcceptFiles:
      return CaptureFileSnapshot(reader, state, output, kind, resource_id);
    case CompilerCommand::PollFiles:
      return PollFileSession(reader, state, output, resource_id);
    case CompilerCommand::WatchFiles:
      return TrackFileSources(reader, state, output, resource_id);
    case CompilerCommand::ReleaseFiles:
      if (reader.remaining() != 0 || state.files.erase(resource_id) != 1) {
        return std::unexpected("unknown file session release");
      }
      return {};
  }
  return std::unexpected("unknown compiler command");
}

}  // namespace

int RunCompilerService() {
  auto layout = native::TargetDataLayout();
  if (!layout) {
    std::println(stderr, "{}", layout.error());
    return 1;
  }
  BinaryWriter hello;
  hello.Write(kCompilerBuildIdentity);
  hello.Write(runtime::NativeRuntimeAbiIdentity());
  hello.Write(runtime::NativeTargetIdentity());
  hello.Write(*layout);
  if (const auto written = WriteCompilerFrame(stdout, hello.bytes()); !written) {
    std::println(stderr, "{}", written.error());
    return 1;
  }
  ServiceOutput messages;
  ServiceState state{
      .output = &messages,
      .programs = {},
      .sources = {},
      .files = {},
      .candidates = {},
  };
  while (true) {
    auto request = ReadCompilerFrame(stdin);
    if (!request) {
      const bool at_end = std::feof(stdin) != 0;
      if (!at_end) {
        std::println(stderr, "{}", request.error());
      }
      return at_end ? 0 : 1;
    }
    BinaryWriter output;
    const auto response = HandleRequest(*request, state, output);
    if (!response) {
      output = BinaryWriter{};
      output.Write(response.error());
    }
    // 帧尾状态允许接收端直接 pop_back 后转移整个负载。
    output.Write(static_cast<std::uint8_t>(response.has_value()));
    output.Write(static_cast<std::uint8_t>(CompilerMessage::Response));
    if (const auto sent = messages.Send(output.bytes()); !sent) {
      std::println(stderr, "{}", sent.error());
      return 1;
    }
  }
}

}  // namespace vela::compiler::jit
