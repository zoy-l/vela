#pragma once

#include <filesystem>
#include <functional>
#include <system_error>

namespace vela::internal {

struct FilePublication final {
  std::reference_wrapper<const std::filesystem::path> temporary;
  std::reference_wrapper<const std::filesystem::path> destination;
};

[[nodiscard]] auto PublishFile(FilePublication publication) -> std::error_code;

}  // namespace vela::internal
