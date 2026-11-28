#include "compiler/jit/external_sources.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>

#include "base/binary.h"
#include "base/declaration.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/modules/source_progress.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/compiler_process.h"
#include "compiler/jit/compiler_protocol.h"

namespace vela::compiler {
namespace {

struct SourceLease final {
  SourceLease(std::shared_ptr<jit::CompilerProcess> process, std::uint64_t resource_id)
      : process(std::move(process)), resource_id(resource_id) {}
  SourceLease(const SourceLease&) = delete;
  SourceLease& operator=(const SourceLease&) = delete;
  SourceLease(SourceLease&&) = delete;
  SourceLease& operator=(SourceLease&&) = delete;
  ~SourceLease() { process->Release(jit::CompilerCommand::ReleaseSources, resource_id); }
  std::shared_ptr<jit::CompilerProcess> process;
  std::uint64_t resource_id;
};
struct CandidateLease final {
  CandidateLease(std::shared_ptr<SourceLease> source, std::uint64_t token)
      : source(std::move(source)), token(token) {}
  CandidateLease(const CandidateLease&) = delete;
  CandidateLease& operator=(const CandidateLease&) = delete;
  CandidateLease(CandidateLease&&) = delete;
  CandidateLease& operator=(CandidateLease&&) = delete;
  ~CandidateLease() { source->process->Release(jit::CompilerCommand::ReleaseCandidate, token); }
  std::shared_ptr<SourceLease> source;
  std::uint64_t token;
};

}  // namespace

class ExternalSourceSession final : public SourceSession {
 public:
  ExternalSourceSession(const std::string& entry,
                        std::shared_ptr<jit::CompilerProcess> process,
                        std::uint64_t resource_id)
      : SourceSession(entry, External{}),
        lease_(std::make_shared<SourceLease>(std::move(process), resource_id)) {}
  std::expected<SourceCandidate, SourceCandidateError> Prepare(
      SourceUpdate update,
      std::span<const DeclarationId> retained,
      const frontend::SourceProgressCallback& progress) const override {
    const auto failure = [](std::string message) {
      return std::unexpected(SourceCandidateError{
          .diagnostics = jit::CompilationDiagnostic(std::move(message)),
          .declaration = {},
      });
    };
    for (const auto& change : update.changes) {
      if (change.syntax) {
        return failure("External source sessions accept file text, not prepared syntax trees");
      }
    }
    auto base = snapshot();
    auto request = jit::CompilerRequest(jit::CompilerCommand::PrepareSources, lease_->resource_id);
    jit::EncodeSourceRequest(request, {
                                          .committed = base->compiler_token,
                                          .update = std::move(update),
                                          .retained = {retained.begin(), retained.end()},
                                          .report_progress = static_cast<bool>(progress),
                                      });
    auto response = lease_->process->Exchange(request.bytes(), progress);
    if (!response) {
      return failure(std::move(response.error()));
    }
    auto decoded = jit::DecodeSourceResponse(*response);
    if (!decoded) {
      return failure(std::move(decoded.error()));
    }
    if (auto* error = std::get_if<SourceCandidateError>(&*decoded)) {
      return std::unexpected(std::move(*error));
    }
    auto& prepared = std::get<jit::PreparedSourceDescription>(*decoded);
    auto result = std::make_shared<CompiledSourceSnapshot>();
    result->session = base->session;
    result->revision = prepared.revision;
    result->changed_sources = std::move(prepared.changed_sources);
    result->sources = base->sources;
    for (auto& source : prepared.sources) {
      if (source.text) {
        result->sources.insert_or_assign(
            std::move(source.path), std::make_shared<const std::string>(std::move(*source.text)));
      } else {
        result->sources.erase(source.path);
      }
    }
    result->roots = std::move(prepared.roots);
    result->standard_names = std::move(prepared.standard_names);
    result->changes = std::move(prepared.changes);
    result->diagnostics = std::move(prepared.diagnostics);
    result->reflection = std::move(prepared.reflection);
    result->identities = frontend::DeclarationIds::FromRecords(std::move(prepared.identities));
    result->timings = {
        .loading = std::chrono::nanoseconds(prepared.loading),
        .checking = std::chrono::nanoseconds(prepared.checking),
        .ir = std::chrono::nanoseconds(prepared.ir),
    };
    result->compiler_token = prepared.token;
    result->compiler_lease = std::make_shared<CandidateLease>(lease_, prepared.token);
    return SourceCandidate(std::move(base), std::move(result));
  }

 private:
  std::shared_ptr<SourceLease> lease_;
};

namespace {

class ExternalDevelopmentFiles final : public DevelopmentFiles {
 public:
  ExternalDevelopmentFiles(std::shared_ptr<jit::CompilerProcess> process,
                           std::uint64_t resource_id,
                           std::string entry)
      : process_(std::move(process)), id_(resource_id), entry_(std::move(entry)) {}
  ~ExternalDevelopmentFiles() override {
    process_->SetFileChanged(id_, {});
    process_->Release(jit::CompilerCommand::ReleaseFiles, id_);
  }
  ExternalDevelopmentFiles(const ExternalDevelopmentFiles&) = delete;
  ExternalDevelopmentFiles& operator=(const ExternalDevelopmentFiles&) = delete;
  ExternalDevelopmentFiles(ExternalDevelopmentFiles&&) = delete;
  ExternalDevelopmentFiles& operator=(ExternalDevelopmentFiles&&) = delete;
  std::expected<void, frontend::Diagnostics> StartWatching(std::function<void()> changed) override {
    auto request = jit::CompilerRequest(jit::CompilerCommand::StartWatching, id_);
    request.Write(static_cast<std::uint8_t>(static_cast<bool>(changed)));
    process_->SetFileChanged(id_, std::move(changed));
    auto response = Call(request);
    if (!response) {
      process_->SetFileChanged(id_, {});
      return std::unexpected(std::move(response.error()));
    }
    entry_ = std::move(response->entry);
    return {};
  }
  std::expected<SourceUpdate, frontend::Diagnostics> Capture(
      const CompiledSourceSnapshot& previous) override {
    auto request = jit::CompilerRequest(jit::CompilerCommand::CaptureFiles, id_);
    request.Write(previous.compiler_token);
    auto response = Call(request);
    if (!response) {
      return std::unexpected(std::move(response.error()));
    }
    if (!response->update) {
      return std::unexpected(jit::CompilationDiagnostic("missing source capture"));
    }
    return std::move(*response->update);
  }
  std::expected<void, frontend::Diagnostics> Accept(
      std::shared_ptr<const CompiledSourceSnapshot> snapshot) override {
    auto request = jit::CompilerRequest(jit::CompilerCommand::AcceptFiles, id_);
    request.Write(snapshot->compiler_token);
    auto accepted = Call(request);
    if (!accepted) {
      return std::unexpected(std::move(accepted.error()));
    }
    // 保留对应编译候选的租约，覆盖下一次按 compiler_token 捕获文件的期间。
    accepted_ = std::move(snapshot);
    return {};
  }
  std::expected<std::optional<SourceUpdate>, frontend::Diagnostics> Poll(
      std::chrono::milliseconds timeout) override {
    auto request = jit::CompilerRequest(jit::CompilerCommand::PollFiles, id_);
    request.Write(static_cast<std::uint64_t>(timeout.count()));
    auto response = Call(request);
    if (!response) {
      return std::unexpected(std::move(response.error()));
    }
    return std::move(response->update);
  }
  std::expected<void, frontend::Diagnostics> WatchSources(
      std::span<const SourceChange> changes) override {
    auto request = jit::CompilerRequest(jit::CompilerCommand::WatchFiles, id_);
    jit::EncodeSourceUpdate(
        request, SourceUpdate{.changes = {changes.begin(), changes.end()}, .added_roots = {}});
    auto response = Call(request);
    if (!response) {
      return std::unexpected(std::move(response.error()));
    }
    return {};
  }
  [[nodiscard]] std::string entry() const override { return entry_; }

 private:
  std::expected<jit::FileOperationResponse, frontend::Diagnostics> Call(
      const BinaryWriter& request) {
    auto response = process_->Exchange(request.bytes());
    if (!response) {
      return std::unexpected(jit::CompilationDiagnostic(std::move(response.error())));
    }
    auto decoded = jit::DecodeFileResponse(*response);
    if (!decoded) {
      return std::unexpected(jit::CompilationDiagnostic(std::move(decoded.error())));
    }
    if (!decoded->diagnostics.empty()) {
      return std::unexpected(std::move(decoded->diagnostics));
    }
    return std::move(*decoded);
  }
  std::shared_ptr<jit::CompilerProcess> process_;
  std::uint64_t id_;
  std::string entry_;
  std::shared_ptr<const CompiledSourceSnapshot> accepted_;
};

}  // namespace

namespace jit {
namespace {

std::expected<void, frontend::Diagnostics> CreateSourceResource(
    CompilerProcess& process,
    CompilerCommand command,
    std::uint64_t resource_id,
    const SourceSessionConfiguration& configuration) {
  auto request = CompilerRequest(command, resource_id);
  EncodeSourceConfiguration(request, configuration);
  auto created = process.Exchange(request.bytes());
  if (!created) {
    return std::unexpected(CompilationDiagnostic(std::move(created.error())));
  }
  return {};
}

}  // namespace

std::expected<std::unique_ptr<DevelopmentFiles>, frontend::Diagnostics> CreateExternalFiles(
    std::shared_ptr<CompilerProcess> process,
    std::uint64_t resource_id,
    const std::string& entry,
    const frontend::ModulePaths& imports) {
  const auto created = CreateSourceResource(
      *process, CompilerCommand::CreateFiles, resource_id,
      {.entry = entry, .diagnostics = {}, .reflection = {}, .imports = imports});
  if (!created) {
    return std::unexpected(created.error());
  }
  return std::make_unique<ExternalDevelopmentFiles>(std::move(process), resource_id, entry);
}

std::expected<std::shared_ptr<SourceSession>, frontend::Diagnostics> CreateExternalSources(
    std::shared_ptr<CompilerProcess> process,
    std::uint64_t resource_id,
    const SourceSessionConfiguration& configuration) {
  const auto created =
      CreateSourceResource(*process, CompilerCommand::CreateSources, resource_id, configuration);
  if (!created) {
    return std::unexpected(created.error());
  }
  return std::make_shared<ExternalSourceSession>(configuration.entry, std::move(process),
                                                 resource_id);
}

}  // namespace jit
}  // namespace vela::compiler
