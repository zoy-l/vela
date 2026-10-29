#include "compiler/source_session.h"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "base/path.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/specialization.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/preparation.h"

namespace vela::compiler {
namespace {

std::string Normalize(std::string_view path) {
  return PathText(Utf8Path(path).lexically_normal());
}

SourceCandidateError SourceError(frontend::Diagnostics diagnostics) {
  return {.status = SourceCandidateStatus::SourceError, .diagnostics = std::move(diagnostics)};
}

}  // namespace

SourceCandidate::SourceCandidate(std::shared_ptr<const SourceSessionIdentity> session,
                                 std::shared_ptr<const CompiledSourceSnapshot> base,
                                 std::shared_ptr<const CompiledSourceSnapshot> prepared)
    : session_(std::move(session)), base_(std::move(base)), prepared_(std::move(prepared)) {}

SourceSession::SourceSession(std::string entry)
    : identity_(std::make_shared<const SourceSessionIdentity>()) {
  auto initial = std::make_shared<CompiledSourceSnapshot>();
  initial->roots.push_back(Normalize(entry));
  current_ = std::move(initial);
}

std::shared_ptr<const CompiledSourceSnapshot> SourceSession::snapshot() const noexcept {
  const std::lock_guard lock(mutex_);
  return current_;
}

std::expected<SourceCandidate, SourceCandidateError> SourceSession::Prepare(
    SourceUpdate update) const {
  auto base = snapshot();
  if (update.base_revision != base->revision) {
    return std::unexpected(SourceCandidateError{
        .status = SourceCandidateStatus::Stale,
        .diagnostics = {},
    });
  }
  auto candidate = std::make_shared<CompiledSourceSnapshot>();
  candidate->revision = base->revision + 1;
  candidate->sources = base->sources;
  candidate->roots = base->roots;
  candidate->identities = base->identities;
  absl::flat_hash_set<std::string> changed;
  for (auto& change : update.changes) {
    const auto path = Normalize(change.path);
    if (!changed.insert(path).second) {
      return std::unexpected(SourceError({frontend::Diagnostic{
          .location = {},
          .message = "duplicate source change",
          .source_path = path,
      }}));
    }
    if (change.text) {
      candidate->sources.insert_or_assign(
          path, std::make_shared<const std::string>(std::move(*change.text)));
    } else {
      candidate->sources.erase(path);
      std::erase(candidate->roots, path);
    }
  }
  absl::flat_hash_set<std::string> roots(candidate->roots.begin(), candidate->roots.end());
  for (const auto& root : update.added_roots) {
    const auto path = Normalize(root);
    if (roots.insert(path).second) {
      candidate->roots.push_back(path);
    }
  }
  std::vector<frontend::SourceInput> inputs;
  inputs.reserve(candidate->sources.size());
  for (const auto& [path, text] : candidate->sources) {
    inputs.push_back({.path = path, .text = *text});
  }
  auto parsed = frontend::LoadSources(inputs, candidate->roots, base->standard_names);
  if (!parsed.diagnostics.empty()) {
    return std::unexpected(SourceError(std::move(parsed.diagnostics)));
  }
  candidate->standard_names = parsed.program.standard_names;
  auto specialized = frontend::SpecializeGenerics(parsed.program);
  if (!specialized.empty()) {
    return std::unexpected(SourceError(std::move(specialized)));
  }
  auto checked = frontend::TypeChecker().Check(parsed.program);
  if (!checked.diagnostics.empty()) {
    return std::unexpected(SourceError(std::move(checked.diagnostics)));
  }
  auto prepared = ir::PrepareProgram(parsed.program, checked.model,
                                     {.declarations = &candidate->identities,
                                      .revision = candidate->revision,
                                      .native_mode = NativeProgramMode::Reloadable});
  if (!prepared.Succeeded()) {
    return std::unexpected(SourceError(std::move(prepared.diagnostics)));
  }
  candidate->program = std::move(prepared.program);
  return SourceCandidate(identity_, std::move(base), std::move(candidate));
}

std::expected<void, SourceCandidateStatus> SourceSession::Commit(const SourceCandidate& candidate) {
  if (candidate.session_ != identity_) {
    return std::unexpected(SourceCandidateStatus::DifferentSession);
  }
  const std::lock_guard lock(mutex_);
  if (current_ != candidate.base_) {
    return std::unexpected(SourceCandidateStatus::Stale);
  }
  current_ = candidate.prepared_;
  return {};
}

}  // namespace vela::compiler
