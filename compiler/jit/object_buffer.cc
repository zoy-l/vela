#include "compiler/jit/object_buffer.h"

#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/MemoryBuffer.h>

namespace vela::compiler::jit {
namespace {

class OwnedObjectBuffer final : public llvm::MemoryBuffer {
 public:
  OwnedObjectBuffer(std::vector<std::byte> bytes, std::size_t offset) : bytes_(std::move(bytes)) {
    const auto payload = std::span(bytes_).subspan(offset);
    const void* storage = payload.data();
    const llvm::StringRef text(static_cast<const char*>(storage), payload.size());
    init(text.begin(), text.end(), false);
  }
  [[nodiscard]] llvm::StringRef getBufferIdentifier() const override {
    return "vela.compiler.object";
  }
  [[nodiscard]] BufferKind getBufferKind() const override { return MemoryBuffer_Malloc; }

 private:
  std::vector<std::byte> bytes_;
};

}  // namespace

std::unique_ptr<llvm::MemoryBuffer> OwnObjectBytes(std::vector<std::byte> bytes,
                                                   std::size_t offset) {
  return std::make_unique<OwnedObjectBuffer>(std::move(bytes), offset);
}

}  // namespace vela::compiler::jit
