#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "compiler/ir/ir.h"

namespace llvm {
class Function;
class Module;
class Constant;
}  // namespace llvm

namespace vela::compiler::native {

// 只负责不可变数据及其重定位，不创建运行时对象或执行用户代码。
struct EmittedNativeImage final {
  std::string symbol;
  llvm::Constant* data = nullptr;
};

std::expected<EmittedNativeImage, std::string> EmitNativeImage(
    llvm::Module& module,
    const ir::Program& program,
    std::span<llvm::Function* const> entries,
    std::span<const std::uint8_t> exports,
    std::string_view module_name);

}  // namespace vela::compiler::native
