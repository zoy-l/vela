#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <yyjson.h>

#include "base/json.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/tooling/lsp/language_server.h"
#include "compiler/tooling/semantic/semantic_index.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling::internal {

namespace protocol {
inline constexpr std::int64_t kInvalidParams = -32602;
inline constexpr std::string_view kJsonRpc = "jsonrpc";
inline constexpr std::string_view kId = "id";
inline constexpr std::string_view kMethod = "method";
inline constexpr std::string_view kParams = "params";
inline constexpr std::string_view kResult = "result";
inline constexpr std::string_view kTextDocument = "textDocument";
inline constexpr std::string_view kUri = "uri";
inline constexpr std::string_view kVersion = "version";
inline constexpr std::string_view kText = "text";
inline constexpr std::string_view kRange = "range";
inline constexpr std::string_view kStart = "start";
inline constexpr std::string_view kEnd = "end";
inline constexpr std::string_view kLine = "line";
inline constexpr std::string_view kCharacter = "character";
inline constexpr std::string_view kDiagnostics = "diagnostics";
inline constexpr std::string_view kPosition = "position";
inline constexpr std::string_view kNewName = "newName";
inline constexpr std::string_view kNewText = "newText";
inline constexpr std::string_view kMessage = "message";
inline constexpr std::string_view kSdkSource = "vela/sdkSource";
}  // namespace protocol

[[nodiscard]] std::optional<std::string_view> JsonText(const yyjson_val* value);
[[nodiscard]] std::optional<std::int64_t> JsonInteger(const yyjson_val* value);
[[nodiscard]] std::expected<TextPosition, std::string> ReadTextPosition(const yyjson_val* value);
[[nodiscard]] yyjson_mut_val* JsonPosition(yyjson_mut_doc* document, TextPosition position);

struct ServerDocument final {
  std::string path;
  SourceText source;
  std::int64_t version = 0;
};

struct RpcCall final {
  const yyjson_val* identity = nullptr;
  const yyjson_val* params = nullptr;
};

class LanguageServer final {
 public:
  explicit LanguageServer(LanguageServerStreams streams)
      : input_(&streams.input.get()),
        output_(&streams.output.get()),
        errors_(&streams.errors.get()) {}
  int Run();

 private:
  enum class State : std::uint8_t { New, Initialized, Shutdown };
  enum class SymbolQuery : std::uint8_t { Completion, Definition, Hover, PrepareRename, Rename };
  void Dispatch(const yyjson_val* request);
  void Initialize(RpcCall call);
  void Request(std::string_view method, RpcCall call);
  void Notification(std::string_view method, const yyjson_val* params);
  [[nodiscard]] static JsonOutput Response(const yyjson_val* request_id);
  void Error(const yyjson_val* request_id, std::int64_t code, std::string_view message);
  void Log(std::string_view message);
  void Send(const JsonOutput& message);
  [[nodiscard]] std::expected<void, std::string> Open(const yyjson_val* params);
  [[nodiscard]] std::expected<void, std::string> Change(const yyjson_val* params);
  [[nodiscard]] std::expected<void, std::string> Close(const yyjson_val* params);
  void Format(RpcCall call);
  void Query(RpcCall call, SymbolQuery query);
  void Rename(RpcCall call, SemanticIndex& index, std::size_t offset);
  [[nodiscard]] std::vector<Completion> CompletionsFor(std::string_view uri,
                                                       const ServerDocument& opened,
                                                       std::size_t offset);
  void DocumentLinks(RpcCall call);
  void SdkSource(RpcCall call);
  void SemanticTokens(RpcCall call);
  SemanticIndex& IndexFor(std::string_view uri, const ServerDocument& opened);
  void PublishDiagnostics();
  void ClearDiagnostics(std::string_view uri);

  std::istream* input_;
  std::ostream* output_;
  std::ostream* errors_;
  State state_ = State::New;
  PositionEncoding encoding_ = PositionEncoding::Utf16;
  absl::flat_hash_map<std::string, ServerDocument> documents_;
  absl::flat_hash_set<std::string> published_;
  absl::flat_hash_map<std::string, SemanticIndex> semantic_indices_;
  struct CompletionIndex final {
    std::string text;
    SemanticIndex index;
  };
  absl::flat_hash_map<std::string, CompletionIndex> completion_indices_;
  frontend::ModuleCache modules_;
  std::optional<int> exit_code_;
  std::optional<std::string> output_error_;
};

}  // namespace vela::tooling::internal
