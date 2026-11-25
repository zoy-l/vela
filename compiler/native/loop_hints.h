#pragma once

#include <cstdint>

namespace llvm {
class Loop;
}  // namespace llvm

namespace vela::compiler::native {

inline constexpr const char* kNativeRecordFieldMetadata = "vela.record.field";

enum class LoopUnrollHints : std::uint8_t { Retain, Replace };

[[nodiscard]] bool HasLoopUnrollHints(const llvm::Loop& loop);
void SetLoopUnrollCount(llvm::Loop& loop, unsigned count, LoopUnrollHints hints);

}  // namespace vela::compiler::native
