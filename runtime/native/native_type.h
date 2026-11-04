#pragma once

#include <cstdint>
#include <string_view>

namespace vela::runtime {

enum class NativeKind : std::uint8_t {
  Void,
  Boolean,
  Integer,
  Double,
  String,
  Future,
  CancellationToken,
  Host,
};

// 宿主键借用 codec 的静态类型描述；不保存某个 Isolate 的身份或 C++ 对象地址。
struct NativeType final {
  NativeKind kind = NativeKind::Void;
  std::string_view host_key;
  bool nullable = false;
  const void* cpp_identity = nullptr;
  bool operator==(const NativeType&) const = default;
};

}  // namespace vela::runtime
