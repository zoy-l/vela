#include "base/file.h"
#include "base/file_publication.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// Windows SDK 的入口头设置体系结构与基础类型，叶子 API 头不能单独完成初始化。
#include <windows.h>  // IWYU pragma: keep

#include <errhandlingapi.h>
#include <fileapi.h>
#include <handleapi.h>
#include <minwinbase.h>
#include <minwindef.h>
#include <winbase.h>
#include <winnt.h>

#include "base/path.h"

namespace vela {

std::expected<std::vector<std::byte>, std::string> ReadBinaryFile(
    const std::filesystem::path& path) {
  auto* const file = CreateFileW(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::unexpected(
        "cannot open file '" + PathText(path) + "': " +
        std::error_code(static_cast<int>(GetLastError()), std::system_category()).message());
  }
  const std::unique_ptr<void, decltype(&CloseHandle)> handle(file, &CloseHandle);
  LARGE_INTEGER information;
  if (GetFileSizeEx(handle.get(), &information) == 0) {
    return std::unexpected(
        "cannot determine file size: " +
        std::error_code(static_cast<int>(GetLastError()), std::system_category()).message());
  }
  const auto size = std::bit_cast<std::uint64_t>(information);
  if (std::cmp_greater(size, std::numeric_limits<std::size_t>::max())) {
    return std::unexpected("file exceeds host size limit");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto remaining = std::span(bytes).subspan(offset);
    const auto count = static_cast<DWORD>(
        std::min<std::size_t>(remaining.size(), std::numeric_limits<DWORD>::max()));
    DWORD received = 0;
    if (ReadFile(handle.get(), remaining.data(), count, &received, nullptr) == 0) {
      return std::unexpected(
          "cannot read file: " +
          std::error_code(static_cast<int>(GetLastError()), std::system_category()).message());
    }
    if (received == 0) {
      return std::unexpected("file changed during reading");
    }
    offset += received;
  }
  return bytes;
}

}  // namespace vela

namespace vela::internal {

std::error_code PublishFile(FilePublication publication) {
  std::error_code error;
  const auto destination = std::filesystem::absolute(publication.destination.get(), error);
  if (error) {
    return error;
  }
  const auto& name = destination.native();
  constexpr auto kMaximumNameUnits =
      (std::numeric_limits<DWORD>::max() - sizeof(FILE_RENAME_INFO)) / sizeof(wchar_t);
  if (std::cmp_greater(name.size(), kMaximumNameUnits)) {
    return std::make_error_code(std::errc::filename_too_long);
  }
  auto* const file = CreateFileW(publication.temporary.get().c_str(), DELETE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return {static_cast<int>(GetLastError()), std::system_category()};
  }
  const std::unique_ptr<void, decltype(&CloseHandle)> handle(file, &CloseHandle);
  const auto size = std::max(sizeof(FILE_RENAME_INFO), offsetof(FILE_RENAME_INFO, FileName) +
                                                           ((name.size() + 1) * sizeof(wchar_t)));
  const auto units = (size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
  std::vector<std::max_align_t> storage(units);
  void* buffer = storage.data();
  auto* info = std::construct_at(static_cast<FILE_RENAME_INFO*>(buffer));
  // POSIX 替换语义保持旧句柄有效，避免并发替换触发 Win32 的 delete-pending 拒绝访问。
  const DWORD flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
  const auto bytes = std::as_writable_bytes(std::span(storage));
  std::memcpy(bytes.subspan(offsetof(FILE_RENAME_INFO, Flags)).data(), &flags, sizeof(flags));
  info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
  const auto filename = std::span(&info->FileName[0], name.size() + 1);
  std::ranges::copy(name, filename.begin());
  filename.back() = L'\0';
  if (SetFileInformationByHandle(handle.get(), FileRenameInfoEx, info, static_cast<DWORD>(size)) ==
      0) {
    return {static_cast<int>(GetLastError()), std::system_category()};
  }
  return {};
}

}  // namespace vela::internal
