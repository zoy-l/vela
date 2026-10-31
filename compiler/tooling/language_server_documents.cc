#include "compiler/tooling/language_server_internal.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <yyjson.h>

#include "base/json.h"
#include "base/path.h"
#include "compiler/tooling/file_uri.h"
#include "compiler/tooling/formatter.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling::internal {
namespace {

using protocol::kInvalidParams;
constexpr std::int64_t kRequestFailed = -32803;

std::expected<TextRange, std::string> ChangeRange(const yyjson_val* value) {
  const auto start = ReadTextPosition(GetJsonField(value, protocol::kStart));
  const auto end = ReadTextPosition(GetJsonField(value, protocol::kEnd));
  if (!start || !end) {
    return std::unexpected("invalid document edit range");
  }
  return TextRange{.start = *start, .end = *end};
}

std::expected<void, std::string> ApplyChanges(SourceText& source,
                                              const yyjson_val* changes,
                                              PositionEncoding encoding) {
  if (!yyjson_is_arr(changes)) {
    return std::unexpected("contentChanges must be an array");
  }
  for (std::size_t index = 0; index < yyjson_arr_size(changes); ++index) {
    const auto* change = yyjson_arr_get(changes, index);
    const auto text = JsonText(GetJsonField(change, protocol::kText));
    if (!text) {
      return std::unexpected("document change requires text");
    }
    const auto* range = GetJsonField(change, protocol::kRange);
    if (range == nullptr) {
      auto replaced = SourceText::Create(std::string(*text));
      if (!replaced) {
        return std::unexpected(replaced.error());
      }
      source = std::move(*replaced);
      continue;
    }
    const auto bounds = ChangeRange(range);
    if (!bounds) {
      return std::unexpected(bounds.error());
    }
    const auto replaced = source.Replace(*bounds, *text, encoding);
    if (!replaced) {
      return replaced;
    }
  }
  return {};
}

}  // namespace

std::expected<TextPosition, std::string> ReadTextPosition(const yyjson_val* value) {
  const auto line = JsonInteger(GetJsonField(value, protocol::kLine));
  const auto character = JsonInteger(GetJsonField(value, protocol::kCharacter));
  if (!line || !character || *line < 0 || *character < 0) {
    return std::unexpected("position requires nonnegative line and character");
  }
  return TextPosition{
      .line = static_cast<std::size_t>(*line),
      .character = static_cast<std::size_t>(*character),
  };
}

yyjson_mut_val* JsonPosition(yyjson_mut_doc* document, TextPosition position) {
  auto* value = yyjson_mut_obj(document);
  AddJsonField(document, value, protocol::kLine, yyjson_mut_uint(document, position.line));
  AddJsonField(document, value, protocol::kCharacter,
               yyjson_mut_uint(document, position.character));
  return value;
}

std::expected<void, std::string> LanguageServer::Open(const yyjson_val* params) {
  const auto* document = GetJsonField(params, protocol::kTextDocument);
  const auto uri = JsonText(GetJsonField(document, protocol::kUri));
  const auto text = JsonText(GetJsonField(document, protocol::kText));
  const auto version = JsonInteger(GetJsonField(document, protocol::kVersion));
  if (!uri || !text || !version) {
    return std::unexpected("didOpen requires uri, text and integer version");
  }
  if (documents_.contains(*uri)) {
    return std::unexpected("document is already open");
  }
  const auto file = FileUriPath(*uri);
  if (!file) {
    return std::unexpected(file.error());
  }
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(*file, error);
  if (error) {
    return std::unexpected("cannot resolve document path: " + error.message());
  }
  auto path = PathText(canonical);
  for (const auto& [open_uri, opened] : documents_) {
    if (opened.path == path) {
      return std::unexpected("the same canonical document is already open under another URI");
    }
  }
  auto source = SourceText::Create(std::string(*text));
  if (!source) {
    return std::unexpected(source.error());
  }
  documents_.emplace(
      std::string(*uri),
      ServerDocument{.path = std::move(path), .source = std::move(*source), .version = *version});
  PublishDiagnostics();
  return {};
}

std::expected<void, std::string> LanguageServer::Change(const yyjson_val* params) {
  const auto* document = GetJsonField(params, protocol::kTextDocument);
  const auto uri = JsonText(GetJsonField(document, protocol::kUri));
  const auto version = JsonInteger(GetJsonField(document, protocol::kVersion));
  if (!uri || !version) {
    return std::unexpected("didChange requires uri and integer version");
  }
  const auto found = documents_.find(*uri);
  if (found == documents_.end()) {
    return std::unexpected("document is not open");
  }
  auto& opened = found->second;
  if (*version <= opened.version) {
    return {};
  }
  // 通知包含多个顺序编辑时，只有全部合法才提交版本与文本，避免半次更新。
  auto next = opened.source;
  const auto changed = ApplyChanges(next, GetJsonField(params, "contentChanges"), encoding_);
  if (!changed) {
    return changed;
  }
  opened.source = std::move(next);
  opened.version = *version;
  PublishDiagnostics();
  return {};
}

std::expected<void, std::string> LanguageServer::Close(const yyjson_val* params) {
  const auto uri =
      JsonText(GetJsonField(GetJsonField(params, protocol::kTextDocument), protocol::kUri));
  if (!uri) {
    return std::unexpected("didClose requires a document URI");
  }
  const auto found = documents_.find(*uri);
  if (found == documents_.end()) {
    return std::unexpected("document is not open");
  }
  ClearDiagnostics(*uri);
  // 关闭后重新从磁盘读取；不能把未保存的 overlay 固定成后续依赖快照。
  modules_.sources.erase(found->second.path);
  documents_.erase(found);
  published_.erase(std::string(*uri));
  PublishDiagnostics();
  return {};
}

void LanguageServer::Format(RpcCall call) {
  const auto* request_id = call.identity;
  const auto* params = call.params;
  const auto uri =
      JsonText(GetJsonField(GetJsonField(params, protocol::kTextDocument), protocol::kUri));
  if (!uri) {
    Error(request_id, kInvalidParams, "formatting requires a document URI");
    return;
  }
  const auto found = documents_.find(*uri);
  if (found == documents_.end()) {
    Error(request_id, kInvalidParams, "document is not open");
    return;
  }
  const auto& source = found->second.source;
  const auto formatted = FormatSource(source.text());
  if (!formatted) {
    Error(request_id, kRequestFailed, formatted.error().front().message);
    return;
  }
  auto response = Response(request_id);
  auto* document = response.document();
  auto* edits = yyjson_mut_arr(document);
  if (*formatted != source.text()) {
    auto* edit = yyjson_mut_obj(document);
    auto* range = yyjson_mut_obj(document);
    AddJsonField(document, range, protocol::kStart, JsonPosition(document, {}));
    AddJsonField(document, range, protocol::kEnd,
                 JsonPosition(document, source.PositionAt(source.text().size(), encoding_)));
    AddJsonField(document, edit, protocol::kRange, range);
    AddJsonField(document, edit, "newText",
                 yyjson_mut_strncpy(document, formatted->data(), formatted->size()));
    yyjson_mut_arr_add_val(edits, edit);
  }
  AddJsonField(document, response.root(), protocol::kResult, edits);
  Send(response);
}

}  // namespace vela::tooling::internal
