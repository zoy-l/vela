#include "compiler/tooling/lsp/language_server_internal.h"

#include <cstddef>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <yyjson.h>

#include "base/json.h"
#include "base/path.h"
#include "compiler/tooling/lsp/file_uri.h"
#include "compiler/tooling/semantic/semantic_index.h"
#include "compiler/tooling/semantic/semantic_references.h"
#include "compiler/tooling/source_files.h"

namespace vela::tooling::internal {

std::expected<void, std::string> LanguageServer::InitializeWorkspace(const yyjson_val* params) {
  std::vector<std::string> roots;
  const auto append = [&roots](const yyjson_val* value) -> std::expected<void, std::string> {
    const auto uri = JsonText(value);
    if (!uri) {
      return std::unexpected("workspace folder requires a file URI");
    }
    const auto path = FileUriPath(*uri);
    if (!path) {
      return std::unexpected(path.error());
    }
    roots.push_back(PathText(*path));
    return {};
  };
  const auto* folders = GetJsonField(params, "workspaceFolders");
  if (folders != nullptr && !yyjson_is_null(folders)) {
    if (!yyjson_is_arr(folders)) {
      return std::unexpected("workspaceFolders must be an array or null");
    }
    for (std::size_t index = 0; index < yyjson_arr_size(folders); ++index) {
      const auto added = append(GetJsonField(yyjson_arr_get(folders, index), protocol::kUri));
      if (!added) {
        return std::unexpected(added.error());
      }
    }
  } else if (const auto* uri = GetJsonField(params, "rootUri");
             uri != nullptr && !yyjson_is_null(uri)) {
    const auto added = append(uri);
    if (!added) {
      return std::unexpected(added.error());
    }
  }
  workspace_roots_ = std::move(roots);
  workspace_sources_.reset();
  return {};
}

std::expected<ReferenceIndex*, std::string> LanguageServer::WorkspaceReferences() {
  if (reference_index_) {
    return &*reference_index_;
  }
  if (!workspace_sources_) {
    absl::flat_hash_set<std::string> discovered;
    for (const auto& root : workspace_roots_) {
      const auto sources = DirectorySources(Utf8Path(root));
      if (!sources) {
        return std::unexpected(root + ": " + sources.error());
      }
      for (const auto& source : *sources) {
        discovered.insert(PathText(source));
      }
    }
    workspace_sources_ = std::move(discovered);
  }
  auto paths = *workspace_sources_;
  for (const auto& [uri, document] : documents_) {
    paths.insert(document.path);
  }
  ReferenceIndex references;
  const auto overlays = OverlaysFor({});
  for (const auto& path : paths) {
    const auto uri = SourceUri(path);
    auto found = semantic_indices_.find(uri);
    if (found == semantic_indices_.end()) {
      found = semantic_indices_.try_emplace(uri, path, modules_, overlays, &configurations_).first;
    }
    found->second.AddReferences(references, encoding_);
  }
  return &reference_index_.emplace(std::move(references));
}

void LanguageServer::References(RpcCall call, const SemanticIndex& index, std::size_t offset) {
  const auto* context = GetJsonField(call.params, "context");
  const auto* include = GetJsonField(context, "includeDeclaration");
  if (!yyjson_is_bool(include)) {
    Error(call.identity, protocol::kInvalidParams,
          "references requires a position and context.includeDeclaration boolean");
    return;
  }
  const auto target = index.FindDefinition(offset, encoding_);
  std::vector<Definition> references;
  if (target) {
    const auto workspace = WorkspaceReferences();
    if (!workspace) {
      Error(call.identity, protocol::kRequestFailed, workspace.error());
      return;
    }
    const auto declaration =
        yyjson_get_bool(include) ? ReferenceDeclaration::Include : ReferenceDeclaration::Exclude;
    references = (*workspace)->FindReferences(*target, declaration);
  }
  auto response = Response(call.identity);
  auto* document = response.document();
  auto* result = yyjson_mut_arr(document);
  for (const auto& reference : references) {
    yyjson_mut_arr_add_val(result, JsonLocation(document, reference));
  }
  AddJsonField(document, response.root(), protocol::kResult, result);
  Send(response);
}

}  // namespace vela::tooling::internal
