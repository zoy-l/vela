#include "compiler/source_session.h"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include "compiler/frontend/ast.h"

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "base/path.h"
#include "compiler/frontend/declaration_ids.h"
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
  return {
      .status = SourceCandidateStatus::SourceError,
      .diagnostics = std::move(diagnostics),
      .declaration = {},
  };
}

std::expected<void, SourceCandidateError> CheckDeclarationContracts(
    const CompiledSourceSnapshot& previous,
    const frontend::Program& source) {
  using Key = std::tuple<std::string, DeclarationKind, std::string>;
  absl::flat_hash_map<Key, const frontend::DeclarationOrigin*> origins;
  for (const auto& [name, origin] : source.origins) {
    origins.emplace(Key{origin.module_uri, origin.kind, origin.name}, &origin);
  }
  const auto records = previous.identities.records();
  for (const auto& declaration : previous.program.declarations) {
    const auto& key =
        records.subspan(static_cast<std::size_t>(declaration.identity.value)).front().key;
    if (key.owner == DeclarationId{} || !key.arguments.empty()) {
      continue;
    }
    const auto& owner = records.subspan(static_cast<std::size_t>(key.owner.value)).front().key;
    if (owner.kind != DeclarationKind::Module) {
      continue;
    }
    const auto found = origins.find(Key{owner.name, key.kind, key.name});
    const auto nominal = key.kind == DeclarationKind::Class ||
                         key.kind == DeclarationKind::Interface ||
                         key.kind == DeclarationKind::Enum;
    std::string message;
    if (found == origins.end()) {
      if (nominal) {
        message = "nominal type was deleted or changed kind; Reload is required";
      }
    } else if (declaration.type_parameters != found->second->type_parameters) {
      message = "generic parameter list changed; Reload is required";
    }
    if (!message.empty()) {
      return std::unexpected(SourceCandidateError{
          .status = SourceCandidateStatus::RequiresReload,
          .diagnostics =
              {
                  {
                      .location = {},
                      .message = key.name + ": " + message,
                      .source_path = owner.name,
                  },
              },
          .declaration = declaration.identity,
      });
    }
  }
  return {};
}

std::expected<void, SourceCandidateError> ApplyChanges(CompiledSourceSnapshot& candidate,
                                                       std::vector<SourceChange>& changes) {
  absl::flat_hash_set<std::string> changed;
  for (auto& change : changes) {
    const auto path = Normalize(change.path);
    if (!changed.insert(path).second) {
      return std::unexpected(SourceError({
          frontend::Diagnostic{
              .location = {},
              .message = "duplicate source change",
              .source_path = path,
          },
      }));
    }
    const auto& syntax = change.syntax;
    if (syntax && (!syntax->source || !syntax->diagnostics.empty() || !change.text ||
                   syntax->source->text != *change.text)) {
      return std::unexpected(SourceError({
          {
              .location = {},
              .message = "prepared source does not match its text",
              .source_path = path,
          },
      }));
    }
    if (candidate.prepared_sources.erase(path) != 0 && !syntax) {
      candidate.modules.sources.erase(path);
    }
    if (change.text) {
      candidate.sources.insert_or_assign(
          path, std::make_shared<const std::string>(std::move(*change.text)));
      if (syntax) {
        candidate.modules.sources.insert_or_assign(path, syntax);
        candidate.prepared_sources.insert(path);
      }
    } else {
      candidate.sources.erase(path);
      candidate.modules.sources.erase(path);
      std::erase(candidate.roots, path);
    }
  }
  return {};
}

std::expected<std::vector<std::string>, frontend::Diagnostics> RetainedRoots(
    const CompiledSourceSnapshot& candidate,
    std::span<const DeclarationId> declarations) {
  std::vector<std::string> roots = candidate.roots;
  absl::flat_hash_set<std::string> loaded(roots.begin(), roots.end());
  absl::flat_hash_set<DeclarationId> visited;
  std::vector<DeclarationId> pending(declarations.begin(), declarations.end());
  const auto records = candidate.identities.records();
  const auto append_type = [&](const auto& self, const frontend::TypeIdentity& type) -> void {
    if (type.declaration != DeclarationId{}) {
      pending.push_back(type.declaration);
    }
    for (const auto& argument : type.arguments) {
      self(self, argument);
    }
  };
  while (!pending.empty()) {
    const auto identity = pending.back();
    pending.pop_back();
    if (identity == DeclarationId{} || !visited.insert(identity).second) {
      continue;
    }
    if (identity.value >= records.size()) {
      return std::unexpected(frontend::Diagnostics{
          {
              .location = {},
              .message = "unknown retained declaration identity",
              .source_path = {},
          },
      });
    }
    const auto& record = records.subspan(static_cast<std::size_t>(identity.value)).front();
    if (record.key.kind == DeclarationKind::Module && candidate.sources.contains(record.key.name) &&
        loaded.insert(record.key.name).second) {
      roots.push_back(record.key.name);
    }
    pending.push_back(record.key.owner);
    for (const auto& argument : record.key.arguments) {
      append_type(append_type, argument);
    }
  }
  return roots;
}

}  // namespace

SourceCandidate::SourceCandidate(std::shared_ptr<const SourceSessionIdentity> session,
                                 std::shared_ptr<const CompiledSourceSnapshot> base,
                                 std::shared_ptr<const CompiledSourceSnapshot> prepared)
    : session_(std::move(session)), base_(std::move(base)), prepared_(std::move(prepared)) {}

SourceSession::SourceSession(const std::string& entry)
    : identity_(std::make_shared<const SourceSessionIdentity>()) {
  auto initial = std::make_shared<CompiledSourceSnapshot>();
  initial->roots.push_back(Normalize(entry));
  current_ = std::move(initial);
}

std::shared_ptr<const CompiledSourceSnapshot> SourceSession::snapshot() const noexcept {
  const std::scoped_lock lock(mutex_);
  return current_;
}

std::expected<SourceCandidate, SourceCandidateError> SourceSession::Prepare(
    SourceUpdate update,
    std::span<const DeclarationId> retained) const {
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();
  auto base = snapshot();
  if (update.base_revision != base->revision) {
    return std::unexpected(SourceCandidateError{
        .status = SourceCandidateStatus::Stale,
        .diagnostics = {},
        .declaration = {},
    });
  }
  auto candidate = std::make_shared<CompiledSourceSnapshot>();
  candidate->revision = base->revision + 1;
  candidate->sources = base->sources;
  candidate->prepared_sources = base->prepared_sources;
  candidate->roots = base->roots;
  candidate->identities = base->identities;
  candidate->modules = base->modules;
  const auto changed = ApplyChanges(*candidate, update.changes);
  if (!changed) {
    return std::unexpected(changed.error());
  }
  absl::flat_hash_set<std::string> roots(candidate->roots.begin(), candidate->roots.end());
  for (const auto& root : update.added_roots) {
    const auto path = Normalize(root);
    if (roots.insert(path).second) {
      candidate->roots.push_back(path);
    }
  }
  // 已装入的模块持续属于开发会话；删除导入不会隐式删除其声明，显式源码删除才移除根。
  for (const auto& [path, syntax] : base->modules.sources) {
    if (candidate->sources.contains(path) && roots.insert(path).second) {
      candidate->roots.push_back(path);
    }
  }
  std::vector<frontend::SourceInput> inputs;
  inputs.reserve(candidate->sources.size());
  for (const auto& [path, text] : candidate->sources) {
    inputs.push_back({.path = path, .text = *text});
  }
  auto retained_roots = RetainedRoots(*candidate, retained);
  if (!retained_roots) {
    return std::unexpected(SourceError(std::move(retained_roots.error())));
  }
  auto parsed =
      frontend::LoadSources(inputs, *retained_roots, candidate->modules, base->standard_names);
  if (!parsed.diagnostics.empty()) {
    return std::unexpected(SourceError(std::move(parsed.diagnostics)));
  }
  const auto contracts = CheckDeclarationContracts(*base, parsed.program);
  if (!contracts) {
    return std::unexpected(contracts.error());
  }
  candidate->standard_names = parsed.program.standard_names;
  candidate->timings.loading = Clock::now() - started;
  const auto specializing = Clock::now();
  auto requests = frontend::RetainedSpecializations(parsed.program, base->identities, retained);
  if (!requests) {
    return std::unexpected(SourceError(std::move(requests.error())));
  }
  auto specialized = frontend::SpecializeGenerics(parsed.program, *requests);
  if (!specialized.empty()) {
    return std::unexpected(SourceError(std::move(specialized)));
  }
  candidate->timings.specialization = Clock::now() - specializing;
  const auto checking = Clock::now();
  auto checked = frontend::TypeChecker().Check(parsed.program);
  if (!checked.diagnostics.empty()) {
    return std::unexpected(SourceError(std::move(checked.diagnostics)));
  }
  candidate->timings.checking = Clock::now() - checking;
  const auto lowering = Clock::now();
  auto prepared = ir::PrepareProgram(parsed.program, checked.model,
                                     {
                                         .declarations = &candidate->identities,
                                         .revision = candidate->revision,
                                         .native_mode = NativeProgramMode::Reloadable,
                                     });
  if (!prepared.Succeeded()) {
    return std::unexpected(SourceError(std::move(prepared.diagnostics)));
  }
  candidate->program = std::move(prepared.program);
  candidate->timings.ir = Clock::now() - lowering;
  return SourceCandidate(identity_, std::move(base), std::move(candidate));
}

std::expected<void, SourceCandidateStatus> SourceSession::Commit(const SourceCandidate& candidate) {
  if (candidate.session_ != identity_) {
    return std::unexpected(SourceCandidateStatus::DifferentSession);
  }
  const std::scoped_lock lock(mutex_);
  if (current_ != candidate.base_) {
    return std::unexpected(SourceCandidateStatus::Stale);
  }
  current_ = candidate.prepared_;
  return {};
}

}  // namespace vela::compiler
