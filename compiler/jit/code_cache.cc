#include "compiler/jit/code_cache.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Module.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/raw_ostream.h>

#include "base/binary.h"
#include "base/file.h"
#include "base/fingerprint.h"
#include "compiler/cache_build_identity.h"
#include "compiler/native/code_generator.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::jit {
namespace {

constexpr std::string_view kMagic = "VELAJIT1";

// ORC 拥有 payload 视图，同时持有完整缓存输入；校验后不再复制目标文件字节。
class CachedObjectBuffer final : public llvm::MemoryBuffer {
 public:
  CachedObjectBuffer(std::vector<std::byte> bytes, std::size_t offset) : bytes_(std::move(bytes)) {
    const auto payload = std::span(bytes_).subspan(offset);
    const void* storage = payload.data();
    const llvm::StringRef text(static_cast<const char*>(storage), payload.size());
    init(text.begin(), text.end(), false);
  }
  [[nodiscard]] llvm::StringRef getBufferIdentifier() const override {
    return "vela.cached.object";
  }
  [[nodiscard]] BufferKind getBufferKind() const override { return MemoryBuffer_Malloc; }

 private:
  std::vector<std::byte> bytes_;
};

std::filesystem::path CachePath(const std::filesystem::path& directory, std::string_view key) {
  return directory / (std::string(key) + ".vlobj");
}

CachedObject Decode(std::vector<std::byte> bytes, std::string_view expected_key) {
  const auto invalid = [] {
    return CachedObject{.status = CodeCacheStatus::Invalid, .object = {}};
  };
  BinaryReader reader(bytes);
  const auto magic = reader.Take(kMagic.size());
  const auto key = reader.Text();
  if (!magic || !key || *key != expected_key ||
      !std::ranges::equal(*magic, std::as_bytes(std::span(kMagic)))) {
    return invalid();
  }
  const auto payload = reader.ChecksummedRemainder();
  if (!payload) {
    return invalid();
  }
  const auto offset = bytes.size() - payload->size();
  auto object = std::make_unique<CachedObjectBuffer>(std::move(bytes), offset);
  auto parsed = llvm::object::ObjectFile::createObjectFile(object->getMemBufferRef());
  if (!parsed) {
    llvm::consumeError(parsed.takeError());
    return invalid();
  }
  return {.status = CodeCacheStatus::Hit, .object = std::move(object)};
}

}  // namespace

std::string CodeCacheKey(const llvm::Module& module, NativeOptimization optimization) {
  std::string identity;
  llvm::raw_string_ostream output(identity);
  output << kCompilerBuildIdentity << '\n'
         << LLVM_VERSION_STRING << '\n'
         << runtime::NativeRuntimeAbiIdentity() << '\n'
         << static_cast<std::uint8_t>(optimization) << '\n';
  llvm::WriteBitcodeToFile(module, output);
  return ContentFingerprint(identity).Hex();
}

std::expected<CachedObject, std::string> ReadCodeCache(const std::filesystem::path& directory,
                                                       std::string_view key) {
  const auto path = CachePath(directory, key);
  std::error_code error;
  const auto exists = std::filesystem::exists(path, error);
  if (error) {
    return std::unexpected("cannot inspect JIT code cache: " + error.message());
  }
  if (!exists) {
    return CachedObject{.status = CodeCacheStatus::Miss, .object = {}};
  }
  auto bytes = ReadBinaryFile(path);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  return Decode(std::move(*bytes), key);
}

std::expected<void, std::string> WriteCodeCache(const std::filesystem::path& directory,
                                                std::string_view key,
                                                llvm::MemoryBufferRef object) {
  const auto payload = object.getBuffer();
  const auto bytes = std::as_bytes(std::span(payload.data(), payload.size()));
  BinaryWriter writer;
  writer.Write(std::as_bytes(std::span(kMagic)));
  writer.Write(key);
  writer.WriteChecksummed(bytes);
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return std::unexpected("cannot create JIT code cache: " + error.message());
  }
  return WriteFileAtomically(CachePath(directory, key), writer.bytes());
}

}  // namespace vela::compiler::jit
