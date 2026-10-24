#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

struct LoweringResult final {
  Program program;
  frontend::Diagnostics diagnostics;

  [[nodiscard]] auto Succeeded() const noexcept -> bool { return diagnostics.empty(); }
};

class Lowerer final {
 public:
  Lowerer(const frontend::Program& source, const frontend::SemanticModel& model) noexcept
      : source_(source), model_(model) {}

  // lowering 状态借用固定的 AST 和语义模型；执行上下文不允许复制或迁移。
  ~Lowerer() = default;
  Lowerer(const Lowerer&) = delete;
  auto operator=(const Lowerer&) -> Lowerer& = delete;
  Lowerer(Lowerer&&) = delete;
  auto operator=(Lowerer&&) -> Lowerer& = delete;

  [[nodiscard]] auto Lower() -> LoweringResult;

 private:
  struct Local final {
    RegisterId reg = 0;
    frontend::TypeDescriptor type;
  };

  struct LoweredValue final {
    RegisterId reg = 0;
    frontend::TypeDescriptor type;
  };

  using ClassMap = absl::flat_hash_map<std::string, ClassId>;
  using FunctionMap = absl::flat_hash_map<std::string, std::uint32_t>;
  using LocalMap = absl::flat_hash_map<std::string, Local>;
  struct LoopTargets final {
    std::vector<std::size_t> break_jumps;
    std::vector<std::size_t> continue_jumps;
  };

  void AddDiagnostic(frontend::SourceLocation location, std::string message);
  void BuildClassTable();
  void BuildFunctionTable();
  void LowerFunction(const frontend::Function& source, const frontend::Class* owner = nullptr);
  void LowerStatement(const frontend::Statement& statement);
  void LowerBlock(const frontend::Statement& statement);

  [[nodiscard]] auto LowerExpression(const frontend::Expression& expression,
                                     const frontend::TypeDescriptor* expected = nullptr)
      -> LoweredValue;
  [[nodiscard]] auto LowerAssignment(const frontend::Expression& expression) -> LoweredValue;
  [[nodiscard]] auto LowerConstruction(const frontend::Expression& expression,
                                       const frontend::TypeDescriptor& expected) -> LoweredValue;
  [[nodiscard]] auto LowerMember(const frontend::Expression& expression) -> LoweredValue;
  [[nodiscard]] auto LowerIndex(const frontend::Expression& expression) -> LoweredValue;
  [[nodiscard]] auto LowerCall(const frontend::Expression& expression,
                               const frontend::TypeDescriptor* expected) -> LoweredValue;

  [[nodiscard]] auto FindClass(std::string_view name) const -> const frontend::ClassDescriptor*;
  [[nodiscard]] auto FindLocal(std::string_view name) const -> const Local*;
  [[nodiscard]] auto AllocateRegister(frontend::SourceLocation location) -> RegisterId;
  [[nodiscard]] auto AllocateTemporary(frontend::SourceLocation location) -> RegisterId;
  [[nodiscard]] static auto IsInteger(const frontend::TypeDescriptor& type) -> bool;
  [[nodiscard]] static auto IsBoolean(const frontend::TypeDescriptor& type) -> bool;
  [[nodiscard]] static auto FindField(const frontend::ClassDescriptor& class_info,
                                      std::string_view name) -> std::optional<FieldId>;
  void Emit(Instruction instruction);
  void PushScope();
  void PopScope();
  auto DeclareLocal(std::string_view name,
                    RegisterId reg,
                    frontend::TypeDescriptor type,
                    frontend::SourceLocation location) -> bool;

  void LowerVariableDeclaration(const frontend::Statement& statement);

  void LowerIf(const frontend::Statement& statement);

  void LowerWhile(const frontend::Statement& statement);

  void LowerFor(const frontend::Statement& statement);

  auto LowerIntegerLiteral(const frontend::Expression& expression) -> LoweredValue;
  auto LowerDoubleLiteral(const frontend::Expression& expression) -> LoweredValue;
  auto LowerDoubleBinary(const frontend::Expression& expression,
                         const LoweredValue& left,
                         const LoweredValue& right) -> LoweredValue;

  auto LowerStringLiteral(const frontend::Expression& expression) -> LoweredValue;

  auto LowerArrayLiteral(const frontend::Expression& expression) -> LoweredValue;
  auto RegisterArrayType(const frontend::TypeDescriptor& type) -> std::uint32_t;

  auto LowerShorthandMember(const frontend::Expression& expression,
                            const frontend::TypeDescriptor* expected) -> LoweredValue;

  auto LowerUnary(const frontend::Expression& expression) -> LoweredValue;

  auto LowerBinary(const frontend::Expression& expression) -> LoweredValue;

  auto LowerInvocation(const frontend::Expression& expression,
                       const frontend::FunctionDescriptor& function,
                       std::uint32_t function_id,
                       const LoweredValue* receiver) -> LoweredValue;
  auto LowerLogical(const frontend::Expression& expression) -> LoweredValue;

  const frontend::Program& source_;
  const frontend::SemanticModel& model_;
  Program program_;
  frontend::Diagnostics diagnostics_;
  ClassMap class_ids_;
  FunctionMap function_ids_;
  absl::flat_hash_map<std::string, std::uint32_t> array_type_ids_;
  absl::flat_hash_map<std::uint64_t, std::uint32_t> double_constant_ids_;
  Function* current_function_ = nullptr;
  std::vector<LocalMap> scopes_;
  std::vector<LoopTargets> loops_;
  RegisterId next_register_ = 0;
};

}  // namespace vela::ir
