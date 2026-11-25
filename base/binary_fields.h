#pragma once

#include <tuple>
#include <type_traits>

#include "base/declaration.h"
#include "base/source_location.h"

namespace vela::binary {

// 基础身份与位置的字段顺序由共享二进制契约统一维护。
template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, DeclarationId>
auto Fields(Record& value) {
  return std::tie(value.value);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, FunctionIdentity>
auto Fields(Record& value) {
  return std::tie(value.declaration, value.revision, value.local);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, SourceLocation>
auto Fields(Record& value) {
  return std::tie(value.line, value.column, value.source_id);
}

}  // namespace vela::binary
