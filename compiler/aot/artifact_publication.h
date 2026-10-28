#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>

#include "compiler/aot/native_compiler.h"

namespace vela::compiler::aot {

struct TargetSupport;

enum class LibraryArtifactKind : std::uint8_t { Object, StaticLibrary };

[[nodiscard]] std::expected<void, std::string> PublishNativeLibrary(
    const std::filesystem::path& output,
    const NativeArtifact& artifact,
    std::span<const std::byte> binary,
    LibraryArtifactKind kind,
    const TargetSupport* support = nullptr);

[[nodiscard]] std::expected<void, std::string> VerifyNativeLibrary(
    const std::filesystem::path& manifest);

}  // namespace vela::compiler::aot
