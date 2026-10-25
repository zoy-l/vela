#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vela {

[[nodiscard]] auto ReadBinaryFile(const std::filesystem::path& path)
    -> std::expected<std::vector<std::byte>, std::string>;
// 临时文件位于目标目录；成功替换是原子的，失败时原目标不受影响。
[[nodiscard]] auto WriteFileAtomically(const std::filesystem::path& path,
                                       std::span<const std::byte> bytes)
    -> std::expected<void, std::string>;

}  // namespace vela
