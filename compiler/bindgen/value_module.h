#pragma once

#include <string>

#include <clang/AST/DeclCXX.h>
#include <clang/Frontend/CompilerInstance.h>

#include "compiler/bindgen/collector.h"

namespace vela::bindgen {

// 从原生函数地址导出值接口，递归读取真实参数类型与字段；不要求上游类型依赖 Vela。
void CollectValueModule(clang::CXXRecordDecl& declaration,
                        const std::string& key,
                        clang::CompilerInstance& compiler,
                        Collection& collection);

}  // namespace vela::bindgen
