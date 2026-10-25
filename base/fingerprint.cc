#include "base/fingerprint.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <xxhash.h>

namespace vela {

Fingerprint ContentFingerprint(std::span<const std::byte> bytes) noexcept {
  const auto hash = XXH3_128bits(bytes.data(), bytes.size());
  return {.low = hash.low64, .high = hash.high64};
}

Fingerprint ContentFingerprint(std::string_view text) noexcept {
  return ContentFingerprint(std::as_bytes(std::span(text.data(), text.size())));
}

std::string Fingerprint::Hex() const {
  constexpr std::string_view kDigits = "0123456789abcdef";
  constexpr unsigned kNibbleBits = 4;
  constexpr std::uint8_t kNibbleMask = 0xf;
  XXH128_canonical_t canonical{};
  XXH128_canonicalFromHash(&canonical, {.low64 = low, .high64 = high});
  std::string result;
  result.reserve(sizeof(canonical.digest) * 2);
  for (const auto byte : canonical.digest) {
    result.push_back(kDigits.substr(byte >> kNibbleBits, 1).front());
    result.push_back(kDigits.substr(byte & kNibbleMask, 1).front());
  }
  return result;
}

}  // namespace vela
