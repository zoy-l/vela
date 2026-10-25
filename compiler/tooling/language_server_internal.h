#pragma once

#include <cstdint>
#include <expected>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <yyjson.h>

#include "base/json.h"
#include "compiler/tooling/language_server.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling::internal {

namespace protocol {
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
inline constexpr std::string_view kMessage = "message";
}  // namespace protocol

[[nodiscard]] auto JsonText(const yyjson_val* value) -> std::optional<std::string_view>;
[[nodiscard]] auto JsonInteger(const yyjson_val* value) -> std::optional<std::int64_t>;
[[nodiscard]] auto ReadTextPosition(const yyjson_val* value)
    -> std::expected<TextPosition, std::string>;
[[nodiscard]] auto JsonPosition(yyjson_mut_doc* document, TextPosition position) -> yyjson_mut_val*;

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
  auto Run() -> int;

 private:
  enum class State : std::uint8_t { New, Initialized, Shutdown };
  void Dispatch(const yyjson_val* request);
  void Initialize(RpcCall call);
  void Request(std::string_view method, RpcCall call);
  void Notification(std::string_view method, const yyjson_val* params);
  [[nodiscard]] static auto Response(const yyjson_val* request_id) -> JsonOutput;
  void Error(const yyjson_val* request_id, std::int64_t code, std::string_view message);
  void Log(std::string_view message);
  void Send(const JsonOutput& message);
  [[nodiscard]] auto Open(const yyjson_val* params) -> std::expected<void, std::string>;
  [[nodiscard]] auto Change(const yyjson_val* params) -> std::expected<void, std::string>;
  [[nodiscard]] auto Close(const yyjson_val* params) -> std::expected<void, std::string>;
  void Format(RpcCall call);
  void PublishDiagnostics();
  void ClearDiagnostics(std::string_view uri);

  std::istream* input_;
  std::ostream* output_;
  std::ostream* errors_;
  State state_ = State::New;
  PositionEncoding encoding_ = PositionEncoding::Utf16;
  absl::flat_hash_map<std::string, ServerDocument> documents_;
  absl::flat_hash_set<std::string> published_;
  std::optional<int> exit_code_;
  std::optional<std::string> output_error_;
};

}  // namespace vela::tooling::internal
