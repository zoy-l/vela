#pragma once

#include <span>
#include <string_view>
#include <tuple>

#include "compiler/frontend/modules/source_loader.h"
#include "compiler/ir/lowerer.h"
#include "compiler/ir/preparation.h"

namespace vela::ir::test {

// 所有后端测试经由同一生产加载和准备路径，返回结果拥有后续编译所需的数据。
inline LoweringResult Prepare(std::string_view source) {
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  return PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
}

// 比较推断与显式标注的降级结果时忽略源码位置，保留执行操作与类型/异常边。
inline auto InstructionOperands(const Instruction& instruction) {
  return std::tie(instruction.opcode, instruction.dst, instruction.lhs, instruction.rhs,
                  instruction.immediate, instruction.index, instruction.handler,
                  instruction.array_type, instruction.field_declaration, instruction.binding_receiver,
                  instruction.completes_construction);
}

}  // namespace vela::ir::test
