#include "compiler/tooling/language_server.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include <yyjson.h>

#include "base/json.h"
#include "compiler/build_info.h"
#include "compiler/tooling/json_rpc.h"
#include "compiler/tooling/language_server_internal.h"
#include "compiler/tooling/semantic_index.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling::internal {
namespace {

constexpr std::int64_t kParseError = -32700;
constexpr std::int64_t kInvalidRequest = -32600;
constexpr std::int64_t kMethodNotFound = -32601;
constexpr std::int64_t kInvalidParams = -32602;
constexpr std::int64_t kNotInitialized = -32002;

void WriteServerFailure(std::ostream& output, std::string_view message) {
  output << "vela lsp: " << message << '\n';
}

bool IsUtf8Supported(const yyjson_val* params) {
  const auto* capabilities = GetJsonField(params, "capabilities");
  const auto* general = GetJsonField(capabilities, "general");
  const auto* encodings = GetJsonField(general, "positionEncodings");
  for (std::size_t index = 0; index < yyjson_arr_size(encodings); ++index) {
    if (JsonText(yyjson_arr_get(encodings, index)) == "utf-8") {
      return true;
    }
  }
  return false;
}

}  // namespace

std::optional<std::string_view> JsonText(const yyjson_val* value) {
  if (!yyjson_is_str(value)) {
    return std::nullopt;
  }
  return std::string_view(yyjson_get_str(value), yyjson_get_len(value));
}

std::optional<std::int64_t> JsonInteger(const yyjson_val* value) {
  if (yyjson_is_sint(value)) {
    return yyjson_get_sint(value);
  }
  if (yyjson_is_uint(value) &&
      yyjson_get_uint(value) <=
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return static_cast<std::int64_t>(yyjson_get_uint(value));
  }
  return std::nullopt;
}

JsonOutput LanguageServer::Response(const yyjson_val* request_id) {
  JsonOutput response;
  auto* document = response.document();
  auto* root = response.root();
  AddJsonField(document, root, protocol::kJsonRpc, yyjson_mut_str(document, "2.0"));
  AddJsonField(document, root, protocol::kId,
               request_id != nullptr ? yyjson_val_mut_copy(document, request_id)
                                     : yyjson_mut_null(document));
  return response;
}

void LanguageServer::Send(const JsonOutput& message) {
  const auto text = message.Serialize();
  if (!text) {
    output_error_ = text.error().message;
    return;
  }
  const auto written = WriteRpcMessage(*output_, *text);
  if (!written) {
    output_error_ = written.error();
  }
}

void LanguageServer::Error(const yyjson_val* request_id,
                           std::int64_t code,
                           std::string_view message) {
  auto response = Response(request_id);
  auto* document = response.document();
  auto* error = yyjson_mut_obj(document);
  AddJsonField(document, error, "code", yyjson_mut_sint(document, code));
  AddJsonField(document, error, protocol::kMessage,
               yyjson_mut_strncpy(document, message.data(), message.size()));
  AddJsonField(document, response.root(), "error", error);
  Send(response);
}

void LanguageServer::Log(std::string_view message) {
  JsonOutput notification;
  auto* document = notification.document();
  auto* root = notification.root();
  AddJsonField(document, root, protocol::kJsonRpc, yyjson_mut_str(document, "2.0"));
  AddJsonField(document, root, protocol::kMethod, yyjson_mut_str(document, "window/logMessage"));
  auto* params = yyjson_mut_obj(document);
  AddJsonField(document, params, "type", yyjson_mut_sint(document, 1));
  AddJsonField(document, params, protocol::kMessage,
               yyjson_mut_strncpy(document, message.data(), message.size()));
  AddJsonField(document, root, protocol::kParams, params);
  Send(notification);
}

void LanguageServer::Initialize(RpcCall call) {
  const auto* request_id = call.identity;
  const auto* params = call.params;
  if (state_ != State::New) {
    Error(request_id, kInvalidRequest, "server is already initialized");
    return;
  }
  if (!yyjson_is_obj(params)) {
    Error(request_id, kInvalidParams, "initialize requires an object");
    return;
  }
  encoding_ = IsUtf8Supported(params) ? PositionEncoding::Utf8 : PositionEncoding::Utf16;
  auto response = Response(request_id);
  auto* document = response.document();
  auto* result = yyjson_mut_obj(document);
  auto* capabilities = yyjson_mut_obj(document);
  auto* sync = yyjson_mut_obj(document);
  AddJsonField(document, sync, "openClose", yyjson_mut_bool(document, true));
  AddJsonField(document, sync, "change", yyjson_mut_sint(document, 2));
  auto* save = yyjson_mut_obj(document);
  AddJsonField(document, save, "includeText", yyjson_mut_bool(document, false));
  AddJsonField(document, sync, "save", save);
  AddJsonField(document, capabilities, "textDocumentSync", sync);
  AddJsonField(document, capabilities, "positionEncoding",
               yyjson_mut_str(document, encoding_ == PositionEncoding::Utf8 ? "utf-8" : "utf-16"));
  AddJsonField(document, capabilities, "documentFormattingProvider",
               yyjson_mut_bool(document, true));
  AddJsonField(document, capabilities, "definitionProvider", yyjson_mut_bool(document, true));
  AddJsonField(document, capabilities, "hoverProvider", yyjson_mut_bool(document, true));
  auto* links = yyjson_mut_obj(document);
  AddJsonField(document, links, "resolveProvider", yyjson_mut_bool(document, false));
  AddJsonField(document, capabilities, "documentLinkProvider", links);
  auto* completion = yyjson_mut_obj(document);
  auto* triggers = yyjson_mut_arr(document);
  yyjson_mut_arr_add_str(document, triggers, ".");
  yyjson_mut_arr_add_str(document, triggers, "(");
  yyjson_mut_arr_add_str(document, triggers, ",");
  AddJsonField(document, completion, "triggerCharacters", triggers);
  AddJsonField(document, capabilities, "completionProvider", completion);
  auto* semantic = yyjson_mut_obj(document);
  auto* legend = yyjson_mut_obj(document);
  auto* types = yyjson_mut_arr(document);
  for (const auto type : kSemanticTokenTypes) {
    yyjson_mut_arr_add_strncpy(document, types, type.data(), type.size());
  }
  auto* modifiers = yyjson_mut_arr(document);
  for (const auto modifier : kSemanticTokenModifiers) {
    yyjson_mut_arr_add_strncpy(document, modifiers, modifier.data(), modifier.size());
  }
  AddJsonField(document, legend, "tokenTypes", types);
  AddJsonField(document, legend, "tokenModifiers", modifiers);
  AddJsonField(document, semantic, "legend", legend);
  AddJsonField(document, semantic, "full", yyjson_mut_bool(document, true));
  AddJsonField(document, capabilities, "semanticTokensProvider", semantic);
  AddJsonField(document, result, "capabilities", capabilities);
  auto* info = yyjson_mut_obj(document);
  AddJsonField(document, info, "name",
               yyjson_mut_strncpy(document, kProductName.data(), kProductName.size()));
  AddJsonField(document, info, "version",
               yyjson_mut_strncpy(document, kVersion.data(), kVersion.size()));
  AddJsonField(document, result, "serverInfo", info);
  AddJsonField(document, response.root(), protocol::kResult, result);
  Send(response);
  state_ = State::Initialized;
}

void LanguageServer::Request(std::string_view method, RpcCall call) {
  const auto* request_id = call.identity;
  if (method == "initialize") {
    Initialize(call);
    return;
  }
  if (state_ == State::New) {
    Error(request_id, kNotInitialized, "server has not been initialized");
    return;
  }
  if (state_ == State::Shutdown) {
    Error(request_id, kInvalidRequest, "server has shut down");
    return;
  }
  if (method == "shutdown") {
    auto response = Response(request_id);
    AddJsonField(response.document(), response.root(), protocol::kResult,
                 yyjson_mut_null(response.document()));
    Send(response);
    state_ = State::Shutdown;
  } else if (method == "textDocument/formatting") {
    Format(call);
  } else if (method == "textDocument/completion") {
    Query(call, SymbolQuery::Completion);
  } else if (method == "textDocument/definition") {
    Query(call, SymbolQuery::Definition);
  } else if (method == "textDocument/hover") {
    Query(call, SymbolQuery::Hover);
  } else if (method == "textDocument/documentLink") {
    DocumentLinks(call);
  } else if (method == protocol::kSdkSource) {
    SdkSource(call);
  } else if (method == "textDocument/semanticTokens/full") {
    SemanticTokens(call);
  } else {
    Error(request_id, kMethodNotFound, "method is not supported");
  }
}

void LanguageServer::Notification(std::string_view method, const yyjson_val* params) {
  if (method == "exit") {
    exit_code_ = state_ == State::Shutdown ? 0 : 1;
    return;
  }
  if (state_ != State::Initialized) {
    return;
  }
  std::expected<void, std::string> changed;
  if (method == "textDocument/didOpen") {
    changed = Open(params);
  } else if (method == "textDocument/didChange") {
    changed = Change(params);
  } else if (method == "textDocument/didClose") {
    changed = Close(params);
  } else if (method == "textDocument/didSave" || method == "workspace/didChangeWatchedFiles") {
    modules_ = {};
    PublishDiagnostics();
  }
  // initialized、$/cancelRequest 与未知通知无需响应；本阶段请求按接收顺序同步完成。
  if (!changed) {
    Log(changed.error());
  }
}

void LanguageServer::Dispatch(const yyjson_val* request) {
  const auto* request_id = GetJsonField(request, protocol::kId);
  const auto method = JsonText(GetJsonField(request, protocol::kMethod));
  const auto version = JsonText(GetJsonField(request, protocol::kJsonRpc));
  const auto valid_id = request_id == nullptr || yyjson_is_null(request_id) ||
                        yyjson_is_str(request_id) || JsonInteger(request_id).has_value();
  if (!yyjson_is_obj(request) || version != "2.0" || !method || !valid_id) {
    Error(valid_id ? request_id : nullptr, kInvalidRequest, "invalid JSON-RPC request");
    return;
  }
  const auto* params = GetJsonField(request, protocol::kParams);
  if (request_id == nullptr) {
    Notification(*method, params);
  } else {
    Request(*method, {.identity = request_id, .params = params});
  }
}

int LanguageServer::Run() {
  while (!exit_code_ && !output_error_) {
    auto message = ReadRpcMessage(*input_);
    if (!message) {
      const auto& error = message.error();
      if (error.kind == RpcErrorKind::Json) {
        Error(nullptr, kParseError, error.message);
        continue;
      }
      WriteServerFailure(*errors_, error.message);
      return 1;
    }
    if (!message->has_value()) {
      return state_ == State::Shutdown ? 0 : 1;
    }
    Dispatch(message->value().root());
  }
  if (output_error_) {
    WriteServerFailure(*errors_, *output_error_);
    return 1;
  }
  return *exit_code_;
}

}  // namespace vela::tooling::internal

namespace vela::tooling {
int RunLanguageServer(LanguageServerStreams streams) {
  return internal::LanguageServer(streams).Run();
}
}  // namespace vela::tooling
