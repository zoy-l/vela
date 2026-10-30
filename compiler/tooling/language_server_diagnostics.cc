#include "compiler/tooling/language_server_internal.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <yyjson.h>

#include "base/json.h"
#include "base/path.h"
#include "compiler/frontend/checker.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_file.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/project_config.h"
#include "compiler/tooling/file_uri.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling::internal {
namespace {

struct EditorDiagnostic final {
  TextPosition position;
  std::string message;
  frontend::DiagnosticSeverity severity;
  frontend::DiagnosticCode code;
};

struct OpenSource final {
  std::string_view uri;
  const SourceText* source;
};

JsonOutput DiagnosticNotification(std::string_view uri) {
  JsonOutput output;
  auto* document = output.document();
  auto* root = output.root();
  AddJsonField(document, root, protocol::kJsonRpc, yyjson_mut_str(document, "2.0"));
  AddJsonField(document, root, protocol::kMethod,
               yyjson_mut_str(document, "textDocument/publishDiagnostics"));
  auto* params = yyjson_mut_obj(document);
  AddJsonField(document, params, protocol::kUri,
               yyjson_mut_strncpy(document, uri.data(), uri.size()));
  AddJsonField(document, params, protocol::kDiagnostics, yyjson_mut_arr(document));
  AddJsonField(document, root, protocol::kParams, params);
  return output;
}

yyjson_mut_val* JsonDiagnostic(yyjson_mut_doc* document, const EditorDiagnostic& diagnostic) {
  auto* diagnostic_object = yyjson_mut_obj(document);
  auto* range = yyjson_mut_obj(document);
  AddJsonField(document, range, protocol::kStart, JsonPosition(document, diagnostic.position));
  AddJsonField(document, range, protocol::kEnd, JsonPosition(document, diagnostic.position));
  AddJsonField(document, diagnostic_object, protocol::kRange, range);
  AddJsonField(document, diagnostic_object, "severity",
               yyjson_mut_sint(document,
                               diagnostic.severity == frontend::DiagnosticSeverity::Error ? 1 : 2));
  const auto code = frontend::DiagnosticCodeName(diagnostic.code);
  if (!code.empty()) {
    AddJsonField(document, diagnostic_object, "code",
                 yyjson_mut_strncpy(document, code.data(), code.size()));
  }
  AddJsonField(document, diagnostic_object, "source", yyjson_mut_str(document, "vela"));
  AddJsonField(document, diagnostic_object, protocol::kMessage,
               yyjson_mut_strncpy(document, diagnostic.message.data(), diagnostic.message.size()));
  return diagnostic_object;
}

using DiagnosticBuckets = absl::flat_hash_map<std::string, std::vector<EditorDiagnostic>>;
using DiagnosticKey = std::tuple<std::string, std::uint32_t, std::uint32_t, std::string>;

// 借用本轮打开文档的视图；检查时立即把快照诊断转换为拥有消息文本的编辑器诊断。
class DiagnosticCollection final {
 public:
  DiagnosticCollection(PositionEncoding encoding, frontend::ModuleCache& modules)
      : encoding_(encoding), modules_(&modules) {}
  void AddOpenDocument(std::string_view uri, const ServerDocument& document) {
    overlays_.push_back({.path = document.path, .text = document.source.text()});
    open_sources_.emplace(document.path, OpenSource{.uri = uri, .source = &document.source});
    EnsureUri(uri);
  }
  void EnsureUri(std::string_view uri) { buckets_.try_emplace(uri); }
  void Collect(std::string_view path);
  [[nodiscard]] const DiagnosticBuckets& buckets() const { return buckets_; }

 private:
  PositionEncoding encoding_;
  std::vector<frontend::SourceInput> overlays_;
  absl::flat_hash_map<std::string_view, OpenSource> open_sources_;
  absl::flat_hash_set<DiagnosticKey> emitted_;
  DiagnosticBuckets buckets_;
  frontend::ModuleCache* modules_;
};

void DiagnosticCollection::Collect(std::string_view path) {
  auto loaded = frontend::LoadProgram(path, *modules_, overlays_);
  auto diagnostics = std::move(loaded.diagnostics);
  if (diagnostics.empty()) {
    const auto options = compiler::LoadSourceDiagnosticOptions(Utf8Path(path));
    diagnostics = options
                      ? frontend::CheckProgram(loaded.program, *options).diagnostics
                      : frontend::Diagnostics{compiler::ConfigurationDiagnostic(options.error())};
  }
  if (diagnostics.empty()) {
    return;
  }
  absl::flat_hash_map<std::string_view, const frontend::SourceFile*> sources;
  for (const auto& source : loaded.program.sources) {
    sources.try_emplace(source.path, &source);
  }
  absl::flat_hash_map<std::string_view, SourceLineIndex> indices;
  for (const auto& diagnostic : diagnostics) {
    const auto source_path =
        diagnostic.source_path.empty() ? std::string(path) : diagnostic.source_path;
    if (!emitted_
             .emplace(source_path, diagnostic.location.line, diagnostic.location.column,
                      diagnostic.message)
             .second) {
      continue;
    }
    const auto found = open_sources_.find(source_path);
    const auto uri = found == open_sources_.end() ? FileUri(Utf8Path(source_path))
                                                  : std::string(found->second.uri);
    TextPosition position;
    if (found != open_sources_.end()) {
      position = found->second.source->PositionAt(diagnostic.location, encoding_);
    } else if (const auto source = sources.find(source_path); source != sources.end()) {
      const auto& text = source->second->text;
      const auto index = indices.try_emplace(source->first, text).first;
      position = index->second.PositionAt(text, diagnostic.location, encoding_);
    }
    auto& bucket = buckets_.try_emplace(uri).first->second;
    bucket.push_back({
        .position = position,
        .message = diagnostic.message,
        .severity = diagnostic.severity,
        .code = diagnostic.code,
    });
  }
}

}  // namespace

void LanguageServer::ClearDiagnostics(std::string_view uri) {
  Send(DiagnosticNotification(uri));
}

void LanguageServer::PublishDiagnostics() {
  semantic_indices_.clear();
  completion_indices_.clear();
  DiagnosticCollection collected(encoding_, modules_);
  std::vector<std::string_view> entries;
  for (const auto& [uri, document] : documents_) {
    collected.AddOpenDocument(uri, document);
    entries.push_back(uri);
  }
  std::ranges::sort(entries);
  for (const auto entry : entries) {
    collected.Collect(documents_.at(std::string(entry)).path);
  }
  // 上一轮可能诊断过已移除的依赖；发空数组才能清除客户端残留。
  for (const auto& uri : published_) {
    collected.EnsureUri(uri);
  }
  const auto& buckets = collected.buckets();
  std::vector<std::string_view> uris;
  uris.reserve(buckets.size());
  for (const auto& [uri, diagnostics] : buckets) {
    uris.push_back(uri);
  }
  std::ranges::sort(uris);
  published_.clear();
  for (const auto uri : uris) {
    auto notification = DiagnosticNotification(uri);
    auto* document = notification.document();
    auto* params = GetMutableJsonField(notification.root(), protocol::kParams);
    auto* array = GetMutableJsonField(params, protocol::kDiagnostics);
    const auto opened = documents_.find(uri);
    if (opened != documents_.end()) {
      AddJsonField(document, params, protocol::kVersion,
                   yyjson_mut_sint(document, opened->second.version));
    }
    const auto& diagnostics = buckets.at(std::string(uri));
    for (const auto& diagnostic : diagnostics) {
      yyjson_mut_arr_add_val(array, JsonDiagnostic(document, diagnostic));
    }
    if (!diagnostics.empty()) {
      published_.emplace(uri);
    }
    Send(notification);
  }
}

}  // namespace vela::tooling::internal
