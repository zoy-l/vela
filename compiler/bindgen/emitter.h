#pragma once

#include <expected>
#include <string>
#include <vector>

#include <llvm/Support/JSON.h>

#include "compiler/bindgen/model.h"

namespace vela::bindgen {

// 一次返回全部产物，由命令所有者统一校验并发布生成代次。
[[nodiscard]] std::expected<llvm::json::Object, std::string> Emit(
    const Module& module,
    const std::vector<std::string>& headers,
    const std::string& output_directory);

}  // namespace vela::bindgen
