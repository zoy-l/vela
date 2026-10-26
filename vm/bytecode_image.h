#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "base/binary.h"
#include "runtime/isolate.h"
#include "vm/program.h"

namespace vela::vm {

inline constexpr std::uint32_t kBytecodeImageVersion = 7;

[[nodiscard]] std::expected<std::vector<std::byte>, BinaryError> SaveBytecode(
    const Program& program,
    const runtime::Isolate& isolate);
[[nodiscard]] std::expected<Program, BinaryError> LoadBytecode(std::span<const std::byte> bytes,
                                                               runtime::Isolate& isolate);

}  // namespace vela::vm
