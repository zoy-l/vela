#include "base/file.h"
#include "base/file_publication.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include "base/path.h"

namespace vela {

std::expected<std::vector<std::byte>, std::string> ReadBinaryFile(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return std::unexpected("cannot open file '" + PathText(path) + "'");
  }
  const auto end = static_cast<std::streamoff>(input.tellg());
  if (end < 0 ||
      std::cmp_greater(static_cast<std::uint64_t>(end), std::numeric_limits<std::size_t>::max())) {
    return std::unexpected("cannot determine file size");
  }
  const auto size = static_cast<std::size_t>(end);
  if (std::cmp_greater(size, std::numeric_limits<std::streamsize>::max())) {
    return std::unexpected("file exceeds stream size limit");
  }
  std::vector<std::byte> bytes(size);
  input.seekg(0);
  void* storage = bytes.data();
  if (size != 0 && !input.read(static_cast<char*>(storage), static_cast<std::streamsize>(size))) {
    return std::unexpected("cannot read complete file '" + PathText(path) + "'");
  }
  return bytes;
}

}  // namespace vela

namespace vela::internal {

std::error_code PublishFile(FilePublication publication) {
  std::error_code error;
  std::filesystem::rename(publication.temporary.get(), publication.destination.get(), error);
  return error;
}

}  // namespace vela::internal
