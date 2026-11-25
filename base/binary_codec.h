#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "base/binary.h"

namespace vela {

inline constexpr std::size_t kMaximumBinaryPayloadBytes = std::size_t{128} * 1024 * 1024;

// Records 只定义领域字段；共享层拥有数字、集合、长度与分配边界的编码契约。
template <class Records>
class BinaryEncoder final {
 public:
  explicit BinaryEncoder(BinaryWriter& writer) : writer_(&writer) {}
  template <class T>
  void Write(const T& value) {
    if constexpr (std::is_same_v<T, std::monostate>) {
    } else if constexpr (std::is_same_v<T, bool>) {
      writer_->Write(static_cast<std::uint8_t>(value));
    } else if constexpr (std::is_enum_v<T>) {
      Write(static_cast<std::underlying_type_t<T>>(value));
    } else if constexpr (std::is_integral_v<T>) {
      writer_->Write(static_cast<std::make_unsigned_t<T>>(value));
    } else if constexpr (std::is_same_v<T, double>) {
      writer_->Write(std::bit_cast<std::uint64_t>(value));
    } else if constexpr (std::is_convertible_v<T, std::string_view>) {
      writer_->Write(std::string_view(value));
    } else if constexpr (requires { value.text(); }) {
      writer_->Write(value.text());
    } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
      Write(static_cast<std::uint64_t>(value.size()));
      writer_->Write(std::span(value));
    } else if constexpr (std::is_pointer_v<T>) {
      Write(value != nullptr);
      if (value) {
        Write(*value);
      }
    } else if constexpr (requires { value.values(); }) {
      Write(value.size);
      for (const auto& item : value.values()) {
        Write(item);
      }
    } else if constexpr (requires { Records::Record(value); }) {
      std::apply([&](const auto&... fields) { (Write(fields), ...); }, Records::Record(value));
    } else if constexpr (requires { value.has_value(); }) {
      Write(value.has_value());
      if (value) {
        Write(*value);
      }
    } else if constexpr (requires { value.index(); }) {
      Write(static_cast<std::uint8_t>(value.index()));
      std::visit([&](const auto& item) { Write(item); }, value);
    } else {
      WriteContainer(value);
    }
  }

 private:
  template <class T>
  void WriteContainer(const T& value) {
    Write(static_cast<std::uint64_t>(value.size()));
    for (const auto& item : value) {
      if constexpr (requires {
                      item.first;
                      item.second;
                    }) {
        Write(item.first);
        Write(item.second);
      } else {
        Write(item);
      }
    }
  }
  BinaryWriter* writer_;
};

// 编解码共用 Records 字段策略；Derived 只提供领域存储（如 arena、驻留字符串）。
template <class Derived, class Records>
class BinaryDecoder {
 private:
  explicit BinaryDecoder(std::span<const std::byte> bytes) : reader_(bytes) {}

 public:
  template <class T>
  void Read(T& value) {
    if (!error_.empty()) {
      return;
    }
    auto& derived = static_cast<Derived&>(*this);
    if constexpr (requires { derived.ReadCustom(value); }) {
      derived.ReadCustom(value);
    } else if constexpr (std::is_same_v<T, std::monostate>) {
    } else if constexpr (std::is_same_v<T, bool>) {
      std::uint8_t raw{};
      Read(raw);
      if (raw > 1) {
        error_ = "invalid binary boolean";
      }
      value = raw != 0;
    } else if constexpr (std::is_enum_v<T>) {
      std::underlying_type_t<T> raw{};
      Read(raw);
      value = static_cast<T>(raw);
    } else if constexpr (std::is_integral_v<T>) {
      const auto raw = reader_.Read<std::make_unsigned_t<T>>();
      if (!raw) {
        error_ = raw.error().message;
      } else {
        value = std::bit_cast<T>(*raw);
      }
    } else if constexpr (std::is_same_v<T, double>) {
      std::uint64_t raw{};
      Read(raw);
      value = std::bit_cast<double>(raw);
    } else if constexpr (std::is_same_v<T, std::string>) {
      auto text = reader_.Text();
      if (!text) {
        error_ = text.error().message;
      } else {
        value = std::move(*text);
      }
    } else if constexpr (requires { Records::Record(value); }) {
      std::apply([&](auto&... fields) { (Read(fields), ...); }, Records::Record(value));
    } else if constexpr (requires { value.has_value(); }) {
      bool present{};
      Read(present);
      if (present) {
        value.emplace();
        Read(*value);
      }
    } else if constexpr (requires { value.index(); }) {
      ReadVariant(value);
    } else {
      ReadContainer(value);
    }
  }

 private:
  template <class T>
  void ReadVariant(T& value) {
    std::uint8_t index{};
    Read(index);
    if (index >= std::variant_size_v<T>) {
      error_ = "invalid binary variant";
      return;
    }
    [&]<std::size_t... Indices>(std::index_sequence<Indices...>) {
      ((index == Indices
            ? (value.template emplace<Indices>(), Read(std::get<Indices>(value)), void())
            : void()),
       ...);
    }(std::make_index_sequence<std::variant_size_v<T>>{});
  }
  template <class T>
  void ReadContainer(T& value) {
    const auto count = reader_.Count(1);
    if (!count) {
      error_ = count.error().message;
      return;
    }
    const auto size = *count;
    if (!CanAllocate(size, sizeof(typename T::value_type))) {
      return;
    }
    if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
      const auto bytes = reader_.Take(size);
      if (!bytes) {
        error_ = bytes.error().message;
      } else {
        value.assign(bytes->begin(), bytes->end());
      }
    } else if constexpr (requires { typename T::mapped_type; }) {
      for (std::size_t index = 0; index < size && error_.empty(); ++index) {
        typename T::key_type key{};
        typename T::mapped_type mapped{};
        Read(key);
        Read(mapped);
        value.emplace(std::move(key), std::move(mapped));
      }
    } else {
      value.resize(size);
      for (auto& item : value) {
        Read(item);
      }
    }
  }

 protected:
  [[nodiscard]] BinaryReader& reader() noexcept { return reader_; }
  [[nodiscard]] std::string& error() noexcept { return error_; }
  bool CanAllocate(std::size_t count, std::size_t element_bytes) {
    if (count > (kMaximumBinaryPayloadBytes - allocated_) / element_bytes) {
      error_ = "binary payload exceeds allocation limit";
      return false;
    }
    allocated_ += count * element_bytes;
    return true;
  }
  std::expected<void, std::string> Finish() {
    if (!error_.empty()) {
      return std::unexpected(std::move(error_));
    }
    if (reader_.remaining() != 0) {
      return std::unexpected("trailing binary payload");
    }
    return {};
  }

 private:
  BinaryReader reader_;
  std::string error_;
  std::size_t allocated_ = 0;
  friend Derived;
};

}  // namespace vela
