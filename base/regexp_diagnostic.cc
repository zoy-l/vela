#include "base/regexp_diagnostic.h"

#include <array>
#include <cstddef>
#include <string>

#include <pcre2.h>

namespace vela {

std::string RegExpErrorMessage(int code) {
  constexpr std::size_t kDiagnosticCapacity = 256;
  std::array<PCRE2_UCHAR, kDiagnosticCapacity> text{};
  const auto size = pcre2_get_error_message(code, text.data(), text.size());
  const void* bytes = text.data();
  return size >= 0 ? std::string(static_cast<const char*>(bytes), static_cast<std::size_t>(size))
                   : "PCRE2 error " + std::to_string(code);
}

}  // namespace vela
