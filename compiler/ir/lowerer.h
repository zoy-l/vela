#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

struct LoweringResult final {
  Program program;
  frontend::Diagnostics diagnostics;

  [[nodiscard]] bool Succeeded() const noexcept { return diagnostics.empty(); }
};

class Lowerer final {
 public:
  Lowerer(const frontend::Program& source, const frontend::SemanticModel& model) noexcept
      : source_(source), model_(model) {}

  [[nodiscard]] LoweringResult Lower();

 private:
  struct Local final {
    RegisterId reg = 0;
    frontend::TypeDescriptor type;
  };

  struct LoweredValue final {
    RegisterId reg = 0;
    frontend::TypeDescriptor type;
  };

  using ClassMap = std::unordered_map<std::string, ClassId>;
  using FunctionMap = std::unordered_map<std::string, std::uint32_t>;
  using LocalMap = std::unordered_map<std::string, Local>;

  void AddDiagnostic(frontend::SourceLocation location, std::string message);
  void BuildClassTable();
  void BuildFunctionTable();
  void LowerFunction(const frontend::Function& source);
  void LowerStatement(const frontend::Statement& statement);
  void LowerBlock(const frontend::Statement& statement);

  [[nodiscard]] LoweredValue LowerExpression(const frontend::Expression& expression,
                                             const frontend::TypeDescriptor* expected = nullptr);
  [[nodiscard]] LoweredValue LowerAssignment(const frontend::Expression& left,
                                             const frontend::Expression& right);
  [[nodiscard]] LoweredValue LowerConstruction(const frontend::Expression& expression,
                                               const frontend::TypeDescriptor& expected);
  [[nodiscard]] LoweredValue LowerMember(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerCall(const frontend::Expression& expression,
                                       const frontend::TypeDescriptor* expected);

  [[nodiscard]] const frontend::ClassDescriptor* FindClass(std::string_view name) const;
  [[nodiscard]] const Local* FindLocal(std::string_view name) const;
  [[nodiscard]] RegisterId AllocateRegister(frontend::SourceLocation location);
  [[nodiscard]] RegisterId AllocateTemporary(frontend::SourceLocation location);
  [[nodiscard]] bool IsSameType(const frontend::TypeDescriptor& left,
                                const frontend::TypeDescriptor& right) const;
  [[nodiscard]] bool IsInteger(const frontend::TypeDescriptor& type) const;
  [[nodiscard]] bool IsBoolean(const frontend::TypeDescriptor& type) const;
  [[nodiscard]] std::optional<FieldId> FindField(const frontend::ClassDescriptor& class_info,
                                                 std::string_view name) const;
  void Emit(Instruction instruction);
  void PushScope();
  void PopScope();
  bool DeclareLocal(std::string_view name,
                    RegisterId reg,
                    frontend::TypeDescriptor type,
                    frontend::SourceLocation location);

  const frontend::Program& source_;
  const frontend::SemanticModel& model_;
  Program program_;
  frontend::Diagnostics diagnostics_;
  ClassMap class_ids_;
  FunctionMap function_ids_;
  Function* current_function_ = nullptr;
  std::vector<LocalMap> scopes_;
  RegisterId next_register_ = 0;
};

}  // namespace vela::ir
