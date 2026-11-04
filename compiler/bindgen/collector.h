#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>

#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/Tooling.h>

#include "compiler/bindgen/model.h"

namespace vela::bindgen {

struct Collection final {
  Module module;
  std::set<std::string> headers;
  // 依赖模块用 C++ 规范名称映射到公开类型键；本模块的类型由 AST 建立。
  std::map<std::string, std::string> dependencies;
};

class CollectorFactory final : public clang::tooling::FrontendActionFactory {
 public:
  explicit CollectorFactory(Collection& collection) : collection_(&collection) {}
  std::unique_ptr<clang::FrontendAction> create() override;

 private:
  Collection* collection_;
};

}  // namespace vela::bindgen
