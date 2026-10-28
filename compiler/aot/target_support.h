#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vela::compiler::aot {

enum class NativeOutputKind : std::uint8_t { Executable, Object, StaticLibrary };

struct TargetSupport final {
  std::filesystem::path package;
  std::filesystem::path archiver;
  std::filesystem::path linker;
  std::filesystem::path runtime;
  std::string identity;
  std::vector<std::string> archive_arguments;
  std::vector<std::string> link_arguments;
};

// 清单及其 SDK 输入在编译前验证；工具始终使用包内绝对路径，不搜索 PATH。
[[nodiscard]] std::expected<TargetSupport, std::string> LoadTargetSupport(
    const std::filesystem::path& manifest,
    NativeOutputKind kind);
[[nodiscard]] std::filesystem::path DefaultTargetSupportPath();
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> LinkNativeArtifact(
    const TargetSupport& support,
    std::span<const std::byte> object,
    NativeOutputKind kind);

}  // namespace vela::compiler::aot
