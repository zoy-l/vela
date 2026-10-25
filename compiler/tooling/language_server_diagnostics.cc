#include "compiler/tooling/language_server_internal.h"

#include <algorithm>
#include <cstddef>
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
#include "base/source_location.h"
#include "base/utf8.h"
#include "compiler/frontend/checker.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_file.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/tooling/file_uri.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling::internal {
namespace {

struct EditorDiagnostic final {
  TextPosition position;
  std::string message;
};

struct DiagnosticSource final {
  std::string_view text;
  std::vector<std::size_t> lines{0};
  explicit DiagnosticSource(std::string_view source) : text(source) {
    for (std::size_t index = 0; index < text.size(); ++index) {
      if (text.at(index) == '\n') {
        lines.push_back(index + 1);
      }
    }
  }
  [[nodiscard]] auto Position(SourceLocation location, PositionEncoding encoding) const
      -> TextPosition;
};

auto DiagnosticSource::Position(SourceLocation location, PositionEncoding encoding) const
    -> TextPosition {
  const auto line = std::min(static_cast<std::size_t>(location.line - 1), lines.size() - 1);
  const auto begin = lines.at(line);
  const auto end = line + 1 < lines.size() ? lines.at(line + 1) - 1 : text.size();
  const auto bytes = std::min(static_cast<std::size_t>(location.column - 1), end - begin);
  if (encoding == PositionEncoding::Utf8) {
    return {.line = line, .character = bytes};
  }
  const auto prefix = text.substr(begin, bytes);
  std::size_t character = 0;
  for (std::size_t index = 0; index < prefix.size(); ++character) {
    constexpr unsigned char kAsciiLimit = 0x80;
    constexpr unsigned char kTwoByteFirst = 0xc2;
    constexpr unsigned char kThreeByteFirst = 0xe0;
    constexpr unsigned char kFourByteFirst = 0xf0;
    constexpr unsigned char kLastUtf8Lead = 0xf4;
    const auto lead = static_cast<unsigned char>(prefix.at(index));
    std::size_t width = 1;
    if (lead >= kTwoByteFirst && lead < kThreeByteFirst) {
      width = 2;
    } else if (lead >= kThreeByteFirst && lead < kFourByteFirst) {
      width = 3;
    } else if (lead >= kFourByteFirst && lead <= kLastUtf8Lead) {
      width = 4;
    }
    const auto remaining = prefix.substr(index);
    if (width > remaining.size() || InvalidUtf8Offset(remaining.substr(0, width))) {
      width = 1;
    }
    if (width == 4) {
      ++character;
    }
    // 非法磁盘字节按一个替代字符计数；不能让错误源码破坏协议位置或越界扫描。
    if (lead < kAsciiLimit) {
      width = 1;
    }
    index += width;
  }
  return {.line = line, .character = character};
}

auto DiagnosticNotification(std::string_view uri) -> JsonOutput {
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

auto JsonDiagnostic(yyjson_mut_doc* document, const EditorDiagnostic& diagnostic)
    -> yyjson_mut_val* {
  auto* diagnostic_object = yyjson_mut_obj(document);
  auto* range = yyjson_mut_obj(document);
  AddJsonField(document, range, protocol::kStart, JsonPosition(document, diagnostic.position));
  AddJsonField(document, range, protocol::kEnd, JsonPosition(document, diagnostic.position));
  AddJsonField(document, diagnostic_object, protocol::kRange, range);
  AddJsonField(document, diagnostic_object, "severity", yyjson_mut_sint(document, 1));
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
  explicit DiagnosticCollection(PositionEncoding encoding) : encoding_(encoding) {}
  void AddOpenDocument(std::string_view uri, const ServerDocument& document) {
    overlays_.push_back({.path = document.path, .text = document.source.text()});
    document_uris_.emplace(document.path, uri);
    EnsureUri(uri);
  }
  void EnsureUri(std::string_view uri) { buckets_.try_emplace(uri); }
  void Collect(std::string_view path);
  [[nodiscard]] auto buckets() const -> const DiagnosticBuckets& { return buckets_; }

 private:
  PositionEncoding encoding_;
  std::vector<frontend::SourceInput> overlays_;
  absl::flat_hash_map<std::string_view, std::string_view> document_uris_;
  absl::flat_hash_set<DiagnosticKey> emitted_;
  DiagnosticBuckets buckets_;
};

void DiagnosticCollection::Collect(std::string_view path) {
  auto loaded = frontend::LoadProgram(path, overlays_);
  auto diagnostics = std::move(loaded.diagnostics);
  if (diagnostics.empty()) {
    diagnostics = frontend::CheckProgram(loaded.program).diagnostics;
  }
  if (diagnostics.empty()) {
    return;
  }
  absl::flat_hash_map<std::string_view, DiagnosticSource> sources;
  for (const auto& source : loaded.program.sources) {
    sources.try_emplace(source.path, source.text);
  }
  for (const auto& diagnostic : diagnostics) {
    const auto source_path =
        diagnostic.source_path.empty() ? std::string(path) : diagnostic.source_path;
    if (!emitted_
             .emplace(source_path, diagnostic.location.line, diagnostic.location.column,
                      diagnostic.message)
             .second) {
      continue;
    }
    const auto found = document_uris_.find(source_path);
    const auto uri =
        found == document_uris_.end() ? FileUri(Utf8Path(source_path)) : std::string(found->second);
    const auto source = sources.find(source_path);
    const auto position = source == sources.end()
                              ? TextPosition()
                              : source->second.Position(diagnostic.location, encoding_);
    auto& bucket = buckets_.try_emplace(uri).first->second;
    bucket.push_back({.position = position, .message = diagnostic.message});
  }
}

}  // namespace

void LanguageServer::ClearDiagnostics(std::string_view uri) {
  Send(DiagnosticNotification(uri));
}

void LanguageServer::PublishDiagnostics() {
  semantic_indices_.clear();
  DiagnosticCollection collected(encoding_);
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
