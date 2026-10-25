#pragma once

#include <string>

namespace vela::frontend {

// 一次加载的不可变输入快照：诊断、缓存和编辑工具消费同一份身份与原文。
struct SourceFile final {
  std::string path;
  std::string text;
};

struct FileResolution final {
  std::string requested_path;
  std::string canonical_path;
};

}  // namespace vela::frontend
