#include "compiler/tooling/lsp/language_server_internal.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <yyjson.h>

#include "base/json.h"
#include "base/path.h"
#include "compiler/frontend/builtins/sdk_modules.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/tooling/lsp/file_uri.h"
#include "compiler/tooling/semantic/semantic_index.h"
#include "compiler/tooling/semantic/semantic_syntax.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling::internal {
namespace {

using protocol::kInvalidParams;

std::string SourceUri(std::string_view path) {
  return path.starts_with(frontend::kSdkSourcePrefix) ? std::string(path) : FileUri(Utf8Path(path));
}

yyjson_mut_val* JsonRange(yyjson_mut_doc* document, TextRange range) {
  auto* value = yyjson_mut_obj(document);
  AddJsonField(document, value, protocol::kStart, JsonPosition(document, range.start));
  AddJsonField(document, value, protocol::kEnd, JsonPosition(document, range.end));
  return value;
}

yyjson_mut_val* JsonCompletion(yyjson_mut_doc* document,
                               const Completion& completion,
                               TextRange range) {
  auto* item = yyjson_mut_obj(document);
  AddJsonField(document, item, "label",
               yyjson_mut_strncpy(document, completion.label.data(), completion.label.size()));
  AddJsonField(document, item, "detail",
               yyjson_mut_strncpy(document, completion.detail.data(), completion.detail.size()));
  AddJsonField(document, item, "kind",
               yyjson_mut_uint(document, static_cast<std::uint8_t>(completion.kind)));
  const auto& insertion = completion.insert_text ? *completion.insert_text : completion.label;
  const auto sorting = (completion.insert_text ? "0" : "1") + completion.label;
  AddJsonField(document, item, "sortText",
               yyjson_mut_strncpy(document, sorting.data(), sorting.size()));
  if (!completion.documentation.empty()) {
    auto* documentation = yyjson_mut_obj(document);
    AddJsonField(document, documentation, "kind", yyjson_mut_str(document, "markdown"));
    AddJsonField(document, documentation, "value",
                 yyjson_mut_strncpy(document, completion.documentation.data(),
                                    completion.documentation.size()));
    AddJsonField(document, item, "documentation", documentation);
  }
  auto* edit = yyjson_mut_obj(document);
  AddJsonField(document, edit, protocol::kRange, JsonRange(document, range));
  AddJsonField(document, edit, protocol::kNewText,
               yyjson_mut_strncpy(document, insertion.data(), insertion.size()));
  AddJsonField(document, item, "textEdit", edit);
  return item;
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
  return semantic_indices_.try_emplace(std::string(uri), opened.path, modules_, overlays)
      .first->second;
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
    const auto target = SourceUri(link.path);
    AddJsonField(document, item, "target",
                 yyjson_mut_strncpy(document, target.data(), target.size()));
    AddJsonField(document, item, protocol::kRange, JsonRange(document, link.range));
    AddJsonField(document, item, "tooltip", yyjson_mut_str(document, "Open imported Vela module"));
    yyjson_mut_arr_add_val(result, item);
  }
  AddJsonField(document, response.root(), protocol::kResult, result);
  Send(response);
}

std::vector<Completion> LanguageServer::CompletionsFor(std::string_view uri,
                                                       const ServerDocument& opened,
                                                       std::size_t offset) {
  const auto text = opened.source.text();
  const auto arguments = PrepareArgumentCompletion(text, offset);
  const auto completion_text = arguments ? arguments->source : CompletionSource(text, offset);
  std::vector<frontend::SourceInput> overlays;
  overlays.reserve(documents_.size());
  for (const auto& [open_uri, opened_document] : documents_) {
    overlays.push_back({
        .path = opened_document.path,
        .text = open_uri == uri ? completion_text : opened_document.source.text(),
    });
  }
  auto cached = completion_indices_.find(uri);
  if (cached == completion_indices_.end() || cached->second.text != completion_text) {
    if (cached != completion_indices_.end()) {
      completion_indices_.erase(cached);
    }
    cached = completion_indices_
                 .emplace(std::string(uri),
                          CompletionIndex{
                              .text = completion_text,
                              .index = SemanticIndex(opened.path, modules_, overlays),
                          })
                 .first;
  }
  const auto& index = cached->second.index;
  auto completions = index.Complete(offset);
  if (arguments) {
    std::erase_if(completions, [&arguments](const Completion& item) -> bool {
      return !item.label.starts_with(arguments->prefix);
    });
    for (auto& item : index.CompleteArguments(*arguments)) {
      completions.push_back(std::move(item));
    }
  }
  return completions;
}

void LanguageServer::SemanticTokens(RpcCall call) {
  const auto uri =
      JsonText(GetJsonField(GetJsonField(call.params, protocol::kTextDocument), protocol::kUri));
  if (!uri) {
    Error(call.identity, kInvalidParams, "semantic tokens require a document URI");
    return;
  }
  const auto found = documents_.find(*uri);
  if (found == documents_.end()) {
    Error(call.identity, kInvalidParams, "document is not open");
    return;
  }
  const auto tokens = IndexFor(*uri, found->second).SemanticTokens(encoding_);
  auto response = Response(call.identity);
  auto* document = response.document();
  auto* result = yyjson_mut_obj(document);
  auto* data = yyjson_mut_arr(document);
  TextPosition previous;
  for (const auto& token : tokens) {
    const auto position = token.position;
    yyjson_mut_arr_add_uint(document, data, position.line - previous.line);
    yyjson_mut_arr_add_uint(document, data,
                            position.line == previous.line ? position.character - previous.character
                                                           : position.character);
    yyjson_mut_arr_add_uint(document, data, token.length);
    yyjson_mut_arr_add_uint(document, data, static_cast<std::uint8_t>(token.kind));
    yyjson_mut_arr_add_uint(document, data, token.modifiers);
    previous = position;
  }
  AddJsonField(document, result, "data", data);
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
  if (query == SymbolQuery::Rename) {
    Rename(call, IndexFor(*uri, opened), *offset);
    return;
  }
  auto response = Response(call.identity);
  auto* document = response.document();
  auto* result = yyjson_mut_null(document);
  if (query == SymbolQuery::Completion) {
    const auto completions = CompletionsFor(*uri, opened, *offset);
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
      yyjson_mut_arr_add_val(result, JsonCompletion(document, completion, range));
    }
  } else {
    // 同版本的跳转与悬停复用 AST/语义索引；文档或磁盘依赖变化时统一失效。
    auto& index = IndexFor(*uri, opened);
    if (query == SymbolQuery::PrepareRename) {
      if (const auto declaration = index.PrepareRename(*offset, encoding_)) {
        result = JsonRange(document, declaration->range);
      }
    } else if (query == SymbolQuery::Hover) {
      result = JsonHover(document, index.FindHover(*offset, encoding_));
    } else if (const auto definition = index.FindDefinition(*offset, encoding_)) {
      result = yyjson_mut_obj(document);
      const auto target = SourceUri(definition->path);
      AddJsonField(document, result, protocol::kUri,
                   yyjson_mut_strncpy(document, target.data(), target.size()));
      AddJsonField(document, result, protocol::kRange, JsonRange(document, definition->range));
    }
  }
  AddJsonField(document, response.root(), protocol::kResult, result);
  Send(response);
}

void LanguageServer::Rename(RpcCall call, SemanticIndex& index, std::size_t offset) {
  const auto name = JsonText(GetJsonField(call.params, protocol::kNewName));
  if (!name) {
    Error(call.identity, kInvalidParams, "rename requires a new name");
    return;
  }
  const auto edits = index.RenameSymbol(offset, *name, encoding_);
  if (!edits) {
    Error(call.identity, kInvalidParams, edits.error());
    return;
  }
  auto response = Response(call.identity);
  auto* document = response.document();
  auto* changes = yyjson_mut_obj(document);
  for (const auto& edit : *edits) {
    const auto uri = SourceUri(edit.path);
    auto* replacements = yyjson_mut_obj_get(changes, uri.c_str());
    if (replacements == nullptr) {
      replacements = yyjson_mut_arr(document);
      AddJsonField(document, changes, uri, replacements);
    }
    auto* item = yyjson_mut_obj(document);
    AddJsonField(document, item, protocol::kRange, JsonRange(document, edit.range));
    AddJsonField(document, item, protocol::kNewText,
                 yyjson_mut_strncpy(document, name->data(), name->size()));
    yyjson_mut_arr_add_val(replacements, item);
  }
  auto* result = yyjson_mut_obj(document);
  AddJsonField(document, result, "changes", changes);
  AddJsonField(document, response.root(), protocol::kResult, result);
  Send(response);
}

void LanguageServer::SdkSource(RpcCall call) {
  const auto uri = JsonText(GetJsonField(call.params, protocol::kUri));
  const auto source = uri ? frontend::SdkModuleSource(*uri) : std::nullopt;
  if (!source) {
    Error(call.identity, kInvalidParams, "uri must identify a registered SDK module");
    return;
  }
  auto response = Response(call.identity);
  AddJsonField(response.document(), response.root(), protocol::kResult,
               yyjson_mut_strncpy(response.document(), source->data(), source->size()));
  Send(response);
}

}  // namespace vela::tooling::internal
