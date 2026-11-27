#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vela {

// kExecutable 在 POSIX 上增加执行权限；Windows 没有对应的文件权限位。
enum class FileMode : std::uint8_t { kRegular, kExecutable };

[[nodiscard]] std::expected<std::vector<std::byte>, std::string> ReadBinaryFile(
    const std::filesystem::path& path);
[[nodiscard]] std::expected<std::string, std::string> ReadTextFile(
    const std::filesystem::path& path);
// 临时文件位于目标目录；成功替换是原子的，失败时原目标不受影响。
[[nodiscard]] std::expected<void, std::string> WriteFileAtomically(
    const std::filesystem::path& path,
    std::span<const std::byte> bytes,
    FileMode mode = FileMode::kRegular);

}  // namespace vela
