#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <pcre2.h>

#include "runtime/object.h"
#include "runtime/string.h"

namespace vela::runtime {

struct RegExpOptions final {
  bool multi_line = false;
  bool case_sensitive = true;
  bool dot_all = false;
};

struct RegExpStorage final {
  StringObject* source{};
  pcre2_code* code{};
  RegExpOptions options;
};

struct RegExpCapture final {
  std::size_t byte_start;
  std::size_t byte_end;
  std::size_t start;
  std::size_t end;
};

struct RegExpMatchStorage final {
  HeapObject* pattern;
  StringObject* input;
  std::uint32_t capture_count;

  [[nodiscard]] std::span<RegExpCapture> captures() noexcept {
    return {StorageAfter<RegExpCapture>(*this), capture_count};
  }
};

}  // namespace vela::runtime
