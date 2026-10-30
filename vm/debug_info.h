#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "base/source_location.h"

namespace vela::vm {

// 错误帧拥有文本，不借用 Program；程序销毁后仍可输出诊断。
struct StackFrame final {
  std::string function;
  std::string source_path;
  SourceLocation location;
  std::size_t instruction = 0;
};

struct SourceMapping final {
  std::uint32_t instruction = 0;
  SourceLocation location;
};

// 映射只在源码位置改变时保存；查询不参与普通指令分派。
struct DebugInfo final {
  std::string_view name;
  std::span<const std::string> source_paths;
  std::span<const SourceMapping> mappings;

  [[nodiscard]] const SourceMapping* Locate(std::size_t instruction) const noexcept;
};

}  // namespace vela::vm
