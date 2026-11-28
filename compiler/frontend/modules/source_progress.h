#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace vela::frontend {

enum class SourceProgressKind : std::uint8_t { File, Loading };

struct SourceProgress final {
  SourceProgressKind kind = SourceProgressKind::Loading;
  std::string_view path;
  std::size_t completed = 0;
  std::size_t total = 0;
};

// 在发起编译的线程同步通知，不重入编译；path 只在回调期间有效。
// 文件计数只覆盖已发现的用户模块绑定，Loading 表示其后的整体编译工作。
using SourceProgressCallback = std::function<void(const SourceProgress&)>;

}  // namespace vela::frontend
