#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace vela {

struct BinaryError final {
  std::size_t offset = 0;
  std::string message;
};

// 小端固定宽度数字与带 uint64 长度的 UTF-8 文本；不保存原生指针或 C++ 对象布局。
class BinaryWriter final {
 public:
  template <class T>
    requires(std::is_unsigned_v<T> && !std::is_same_v<T, bool>)
  void Write(T value) {
    constexpr unsigned kByteBits = 8;
    constexpr T kByteMask = 0xff;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
      bytes_.push_back(static_cast<std::byte>(value & kByteMask));
      value = static_cast<T>(value >> kByteBits);
    }
  }
  void Write(std::string_view text);
  void Write(std::span<const std::byte> bytes);
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::vector<std::byte> Take() && noexcept { return std::move(bytes_); }

 private:
  std::vector<std::byte> bytes_;
};

class BinaryReader final {
 public:
  explicit BinaryReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}
  template <class T>
    requires(std::is_unsigned_v<T> && !std::is_same_v<T, bool>)
  [[nodiscard]] std::expected<T, BinaryError> Read() {
    static_assert(sizeof(T) <= sizeof(std::uint64_t));
    const auto bytes = Take(sizeof(T));
    if (!bytes) {
      return std::unexpected(bytes.error());
    }
    constexpr unsigned kByteBits = 8;
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
      result |= std::to_integer<std::uint64_t>(bytes->subspan(index).front())
                << (index * kByteBits);
    }
    return static_cast<T>(result);
  }
  template <class... Types>
  [[nodiscard]] std::expected<std::tuple<Types...>, BinaryError> Record() {
    std::expected<std::tuple<Types...>, BinaryError> record(std::in_place);
    [&]<std::size_t... Indices>(std::index_sequence<Indices...>) -> void {
      (
          [&]() -> void {
            if (!record) {
              return;
            }
            const auto field = Read<std::tuple_element_t<Indices, std::tuple<Types...>>>();
            if (!field) {
              record = std::unexpected(field.error());
            } else {
              std::get<Indices>(*record) = *field;
            }
          }(),
          ...);
    }(std::index_sequence_for<Types...>());
    return record;
  }
  [[nodiscard]] std::expected<std::string, BinaryError> Text();
  [[nodiscard]] std::expected<std::size_t, BinaryError> Count(std::size_t minimum_element_bytes);
  [[nodiscard]] std::expected<std::span<const std::byte>, BinaryError> Take(std::size_t count);
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] BinaryError Error(std::string message) const;

 private:
  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0;
};

}  // namespace vela
