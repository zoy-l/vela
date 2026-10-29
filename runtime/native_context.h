#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "runtime/value.h"

namespace vela::runtime {

class Isolate;
class Future;
class Completer;
class CancellationToken;

enum class NativeErrorKind : std::uint8_t { Failure, TypeMismatch, OutOfMemory, InvalidUtf8 };

struct NativeError final {
  NativeErrorKind kind = NativeErrorKind::Failure;
  std::string message;
};

using NativeResult = std::expected<Value, NativeError>;

class NativeContext final {
 public:
  explicit NativeContext(Isolate& isolate) noexcept : isolate_(&isolate) {}
  [[nodiscard]] Isolate& isolate() const noexcept { return *isolate_; }
  [[nodiscard]] NativeResult String(std::string_view text) const;
  [[nodiscard]] std::expected<Value, NativeError> FutureValue(const Future& future) const;
  [[nodiscard]] Completer CreateCompleter() const;
  [[nodiscard]] NativeResult CancellationValue(const CancellationToken& token) const;

 private:
  Isolate* isolate_;
};

}  // namespace vela::runtime
