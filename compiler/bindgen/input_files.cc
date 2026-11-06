#include "compiler/bindgen/input_files.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/Twine.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>

#include "base/fingerprint.h"

namespace vela::bindgen {
namespace {

class InputFile final : public llvm::vfs::File {
 public:
  InputFile(std::unique_ptr<llvm::vfs::File> file, InputFileSystem& owner, std::string path)
      : file_(std::move(file)), owner_(&owner), path_(std::move(path)) {}
  InputFile(const InputFile&) = delete;
  InputFile& operator=(const InputFile&) = delete;
  InputFile(InputFile&&) = delete;
  InputFile& operator=(InputFile&&) = delete;
  ~InputFile() override { std::ignore = file_->close(); }

  llvm::ErrorOr<llvm::vfs::Status> status() override { return file_->status(); }
  std::error_code close() override { return file_->close(); }
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getBuffer(const llvm::Twine& name,
                                                               std::int64_t file_size,
                                                               bool requires_null_terminator,
                                                               bool is_volatile) override {
    const auto source = file_->getBuffer(name, file_size, requires_null_terminator, is_volatile);
    if (!source) {
      return source.getError();
    }
    // 禁止 mmap 的后续写入改变 Clang 正在解析的内容；摘要描述实际交给解析器的快照。
    auto snapshot = llvm::MemoryBuffer::getMemBufferCopy((*source)->getBuffer(), name.str());
    const auto contents = snapshot->getBuffer();
    const auto fingerprint =
        ContentFingerprint(std::string_view(contents.data(), contents.size())).Hex();
    if (const auto error = owner_->Record(path_, fingerprint)) {
      return error;
    }
    return snapshot;
  }

 private:
  std::unique_ptr<llvm::vfs::File> file_;
  InputFileSystem* owner_;
  std::string path_;
};

}  // namespace

InputFileSystem::InputFileSystem() : ProxyFileSystem(llvm::vfs::getRealFileSystem()) {}

llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> InputFileSystem::openFileForRead(
    const llvm::Twine& path) {
  auto file = ProxyFileSystem::openFileForRead(path);
  if (!file) {
    return file.getError();
  }
  llvm::SmallString<0> real;
  if (const auto error = getRealPath(path, real)) {
    failure_ = error;
    return error;
  }
  auto canonical = std::string(real);
  return std::make_unique<InputFile>(std::move(*file), *this, std::move(canonical));
}

std::error_code InputFileSystem::Record(const std::string& path, const std::string& fingerprint) {
  const auto [previous, inserted] = fingerprints_.emplace(path, fingerprint);
  if (!inserted && previous->second != fingerprint) {
    failure_ = std::make_error_code(std::errc::state_not_recoverable);
  }
  return failure_;
}

}  // namespace vela::bindgen
