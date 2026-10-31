#pragma once

#include <string>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

// 转换器只在克隆期间借用；词法作用域按源码的绑定顺序进入与退出。
class AstCloneTransform {
 public:
  AstCloneTransform() = default;
  virtual ~AstCloneTransform() = default;
  AstCloneTransform(const AstCloneTransform&) = delete;
  AstCloneTransform& operator=(const AstCloneTransform&) = delete;
  AstCloneTransform(AstCloneTransform&&) = delete;
  AstCloneTransform& operator=(AstCloneTransform&&) = delete;
  virtual TypeName TransformType(const TypeName& source) = 0;
  // 仅处理显式类型实参；target 的类型已转换，表达式子节点尚未构造。
  virtual void TransformApplication(Expression& target, const Expression& source) = 0;
  virtual void BeginScope() = 0;
  virtual void EndScope() = 0;
  virtual void Declare(const std::string& name) = 0;
};

// 模板与源码快照保持只读；每个绑定/特化实例拥有独立节点身份。
[[nodiscard]] Function CloneFunction(const Function& source, AstCloneTransform& transform);
[[nodiscard]] Class CloneClass(const Class& source, AstCloneTransform& transform);
// 只复制声明结构与字段初始化表达式；函数体置空，用于只需要成员签名的抽象应用。
[[nodiscard]] Class CloneClassDeclaration(const Class& source, AstCloneTransform& transform);
[[nodiscard]] Interface CloneInterface(const Interface& source, AstCloneTransform& transform);
// 单个解析单元的位置重定位到合并程序或交互提交中的实际起点。
[[nodiscard]] Program CloneProgram(const Program& source, SourceLocation origin);

}  // namespace vela::frontend
