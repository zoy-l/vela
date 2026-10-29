#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace vela::frontend {

struct SourceSnapshot;

// 一次加载的不可变输入快照：诊断、缓存和编辑工具消费同一份身份与原文。
struct SourceFile final {
  std::string path;
  // text 借用 syntax 的不可变文本；加载结果共享原文、token 和导入信息。
  std::string_view text;
  std::shared_ptr<const SourceSnapshot> syntax;
};

struct FileResolution final {
  std::string requested_path;
  std::string canonical_path;
};

}  // namespace vela::frontend
