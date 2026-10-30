#pragma once

#include <expected>
#include <span>
#include <string>
#include <vector>

#include "base/declaration.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/declaration_ids.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

struct GenericSpecialization final {
  std::string name;
  std::vector<TypeName> arguments;
};

// 用当前模块绑定重建存留特化，不能复用旧导入别名产生的扁平名字。
[[nodiscard]] std::expected<std::vector<GenericSpecialization>, Diagnostics>
RetainedSpecializations(const Program& program,
                        const DeclarationIds& identities,
                        std::span<const DeclarationId> declarations);

// 对静态泛型用法生成共享的特化 AST；后续类型检查和后端只消费具体声明。
[[nodiscard]] Diagnostics SpecializeGenerics(Program& program,
                                             std::span<const GenericSpecialization> retained = {});

}  // namespace vela::frontend
