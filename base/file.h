#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vela {

[[nodiscard]] std::expected<std::vector<std::byte>, std::string> ReadBinaryFile(
    const std::filesystem::path& path);
// 临时文件位于目标目录；成功替换是原子的，失败时原目标不受影响。
[[nodiscard]] std::expected<void, std::string> WriteFileAtomically(
    const std::filesystem::path& path,
    std::span<const std::byte> bytes);

}  // namespace vela
