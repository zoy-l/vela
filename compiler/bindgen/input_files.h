#pragma once

#include <map>
#include <memory>
#include <string>
#include <system_error>

#include <llvm/ADT/Twine.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/VirtualFileSystem.h>

namespace vela::bindgen {

class InputFileSystem final : public llvm::vfs::ProxyFileSystem {
 public:
  InputFileSystem();
  llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine& path) override;
  [[nodiscard]] const std::map<std::string, std::string>& fingerprints() const noexcept {
    return fingerprints_;
  }
  [[nodiscard]] std::error_code failure() const noexcept { return failure_; }
  std::error_code Record(const std::string& path, const std::string& fingerprint);

 private:
  std::map<std::string, std::string> fingerprints_;
  std::error_code failure_;
};

}  // namespace vela::bindgen
