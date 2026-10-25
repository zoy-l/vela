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

inline constexpr std::uint32_t kBytecodeImageVersion = 1;

[[nodiscard]] auto SaveBytecode(const Program& program, const runtime::Isolate& isolate)
    -> std::expected<std::vector<std::byte>, BinaryError>;
[[nodiscard]] auto LoadBytecode(std::span<const std::byte> bytes, runtime::Isolate& isolate)
    -> std::expected<Program, BinaryError>;

}  // namespace vela::vm
