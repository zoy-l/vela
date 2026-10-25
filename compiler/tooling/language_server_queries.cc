#include "compiler/tooling/language_server_internal.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <yyjson.h>

#include "base/json.h"
#include "base/path.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/tooling/file_uri.h"
#include "compiler/tooling/semantic_index.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling::internal {
namespace {

constexpr std::int64_t kInvalidParams = -32602;

yyjson_mut_val* JsonRange(yyjson_mut_doc* document, TextRange range) {
  auto* value = yyjson_mut_obj(document);
  AddJsonField(document, value, protocol::kStart, JsonPosition(document, range.start));
  AddJsonField(document, value, protocol::kEnd, JsonPosition(document, range.end));
  return value;
}

bool IsIdentifierByte(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '_';
}

yyjson_mut_val* JsonHover(yyjson_mut_doc* document, const std::optional<Hover>& hover) {
  if (!hover) {
    return yyjson_mut_null(document);
  }
  auto* result = yyjson_mut_obj(document);
  auto* contents = yyjson_mut_obj(document);
  auto markdown = "```vela\n" + hover->signature + "\n```";
  if (!hover->documentation.empty()) {
    markdown += "\n\n" + hover->documentation;
  }
  AddJsonField(document, contents, "kind", yyjson_mut_str(document, "markdown"));
  AddJsonField(document, contents, "value",
               yyjson_mut_strncpy(document, markdown.data(), markdown.size()));
  AddJsonField(document, result, "contents", contents);
  AddJsonField(document, result, protocol::kRange, JsonRange(document, hover->range));
  return result;
}

}  // namespace

SemanticIndex& LanguageServer::IndexFor(std::string_view uri, const ServerDocument& opened) {
  if (const auto found = semantic_indices_.find(uri); found != semantic_indices_.end()) {
    return found->second;
  }
  std::vector<frontend::SourceInput> overlays;
  overlays.reserve(documents_.size());
  for (const auto& [open_uri, document] : documents_) {
    overlays.push_back({.path = document.path, .text = document.source.text()});
  }
  return semantic_indices_.try_emplace(std::string(uri), opened.path, overlays).first->second;
}

void LanguageServer::DocumentLinks(RpcCall call) {
  const auto uri =
      JsonText(GetJsonField(GetJsonField(call.params, protocol::kTextDocument), protocol::kUri));
  if (!uri) {
    Error(call.identity, kInvalidParams, "document links require a document URI");
    return;
  }
  const auto found = documents_.find(*uri);
  if (found == documents_.end()) {
    Error(call.identity, kInvalidParams, "document is not open");
    return;
  }
  const auto links = IndexFor(*uri, found->second).ImportLinks(encoding_);
  auto response = Response(call.identity);
  auto* document = response.document();
  auto* result = yyjson_mut_arr(document);
  for (const auto& link : links) {
    auto* item = yyjson_mut_obj(document);
    const auto target = FileUri(Utf8Path(link.path));
    AddJsonField(document, item, "target",
                 yyjson_mut_strncpy(document, target.data(), target.size()));
    AddJsonField(document, item, protocol::kRange, JsonRange(document, link.range));
    AddJsonField(document, item, "tooltip", yyjson_mut_str(document, "Open imported Vela module"));
    yyjson_mut_arr_add_val(result, item);
  }
  AddJsonField(document, response.root(), protocol::kResult, result);
  Send(response);
}

void LanguageServer::Query(RpcCall call, SymbolQuery query) {
  const auto uri =
      JsonText(GetJsonField(GetJsonField(call.params, protocol::kTextDocument), protocol::kUri));
  const auto position = ReadTextPosition(GetJsonField(call.params, protocol::kPosition));
  if (!uri || !position) {
    Error(call.identity, kInvalidParams, "symbol query requires a document URI and position");
    return;
  }
  const auto found = documents_.find(*uri);
  if (found == documents_.end()) {
    Error(call.identity, kInvalidParams, "document is not open");
    return;
  }
  const auto& opened = found->second;
  const auto& source = opened.source;
  const auto offset = source.OffsetAt(*position, encoding_);
  if (!offset) {
    Error(call.identity, kInvalidParams, offset.error());
    return;
  }
  auto response = Response(call.identity);
  auto* document = response.document();
  auto* result = yyjson_mut_null(document);
  if (query == SymbolQuery::Completion) {
    const auto completion_text = CompletionSource(source.text(), *offset);
    std::vector<frontend::SourceInput> overlays;
    overlays.reserve(documents_.size());
    for (const auto& [open_uri, opened_document] : documents_) {
      overlays.push_back({
          .path = opened_document.path,
          .text = open_uri == *uri ? completion_text : opened_document.source.text(),
      });
    }
    const auto completions = SemanticIndex(opened.path, overlays).Complete(*offset);
    auto begin = *offset;
    auto end = *offset;
    const auto text = source.text();
    while (begin > 0 && IsIdentifierByte(text.at(begin - 1))) {
      --begin;
    }
    while (end < text.size() && IsIdentifierByte(text.at(end))) {
      ++end;
    }
    const TextRange range{
        .start = source.PositionAt(begin, encoding_),
        .end = source.PositionAt(end, encoding_),
    };
    result = yyjson_mut_arr(document);
    for (const auto& completion : completions) {
      auto* item = yyjson_mut_obj(document);
      AddJsonField(document, item, "label",
                   yyjson_mut_strncpy(document, completion.label.data(), completion.label.size()));
      AddJsonField(
          document, item, "detail",
          yyjson_mut_strncpy(document, completion.detail.data(), completion.detail.size()));
      AddJsonField(document, item, "kind",
                   yyjson_mut_uint(document, static_cast<std::uint8_t>(completion.kind)));
      auto* edit = yyjson_mut_obj(document);
      AddJsonField(document, edit, protocol::kRange, JsonRange(document, range));
      AddJsonField(document, edit, "newText",
                   yyjson_mut_strncpy(document, completion.label.data(), completion.label.size()));
      AddJsonField(document, item, "textEdit", edit);
      yyjson_mut_arr_add_val(result, item);
    }
  } else {
    // 同版本的跳转与悬停复用 AST/语义索引；文档或磁盘依赖变化时统一失效。
    auto& index = IndexFor(*uri, opened);
    if (query == SymbolQuery::Hover) {
      result = JsonHover(document, index.FindHover(*offset, encoding_));
    } else if (const auto definition = index.FindDefinition(*offset, encoding_)) {
      result = yyjson_mut_obj(document);
      const auto target = FileUri(Utf8Path(definition->path));
      AddJsonField(document, result, protocol::kUri,
                   yyjson_mut_strncpy(document, target.data(), target.size()));
      AddJsonField(document, result, protocol::kRange, JsonRange(document, definition->range));
    }
  }
  AddJsonField(document, response.root(), protocol::kResult, result);
  Send(response);
}

}  // namespace vela::tooling::internal
