#include "base/binary.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/fingerprint.h"
#include "base/utf8.h"

namespace vela {

void BinaryWriter::Write(std::span<const std::byte> bytes) {
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

void BinaryWriter::Write(std::string_view text) {
  Write(static_cast<std::uint64_t>(text.size()));
  Write(std::as_bytes(std::span(text.data(), text.size())));
}

void BinaryWriter::WriteChecksummed(std::span<const std::byte> payload) {
  const auto checksum = ContentFingerprint(payload);
  Write(checksum.low);
  Write(checksum.high);
  Write(payload);
}

std::expected<std::span<const std::byte>, BinaryError> BinaryReader::Take(std::size_t count) {
  if (count > remaining()) {
    return std::unexpected(Error("truncated binary input"));
  }
  const auto result = bytes_.subspan(offset_, count);
  offset_ += count;
  return result;
}

std::expected<std::string, BinaryError> BinaryReader::Text() {
  const auto length = Read<std::uint64_t>();
  if (!length) {
    return std::unexpected(length.error());
  }
  if (std::cmp_greater(*length, remaining())) {
    return std::unexpected(Error("binary text length exceeds remaining input"));
  }
  const auto data = Take(static_cast<std::size_t>(*length));
  if (!data) {
    return std::unexpected(data.error());
  }
  const void* storage = data->data();
  const std::string_view text(static_cast<const char*>(storage), data->size());
  if (InvalidUtf8Offset(text)) {
    return std::unexpected(Error("binary text is not valid UTF-8"));
  }
  return std::string(text);
}

std::expected<std::size_t, BinaryError> BinaryReader::Count(std::size_t minimum_element_bytes) {
  const auto count = Read<std::uint64_t>();
  if (!count) {
    return std::unexpected(count.error());
  }
  if (*count > remaining() / minimum_element_bytes) {
    return std::unexpected(Error("binary element count exceeds remaining input"));
  }
  return static_cast<std::size_t>(*count);
}

BinaryError BinaryReader::Error(std::string message) const {
  return {.offset = offset_, .message = std::move(message)};
}

std::expected<std::span<const std::byte>, BinaryError> BinaryReader::ChecksummedRemainder() {
  const auto checksum = Record<std::uint64_t, std::uint64_t>();
  if (!checksum) {
    return std::unexpected(checksum.error());
  }
  const auto payload = *Take(remaining());
  const auto [low, high] = *checksum;
  if (ContentFingerprint(payload) != Fingerprint{.low = low, .high = high}) {
    return std::unexpected(Error("binary checksum mismatch"));
  }
  return payload;
}

}  // namespace vela
