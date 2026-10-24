#pragma once

#include <cstdint>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

struct TypeDescriptor final {
  enum class Kind : std::uint8_t {
    Invalid,
    Void,
    Bool,
    Int,
    Float,
    String,
    Array,
    Enum,
    Class,
  };

  Kind kind = Kind::Invalid;
  std::string name;
};

struct FunctionDescriptor final {
  TypeDescriptor return_type;
  std::vector<std::string> parameter_order;
  std::unordered_map<std::string, TypeDescriptor> parameters;
};

struct ClassDescriptor final {
  TypeDescriptor type;
  std::vector<std::string> field_order;
  std::unordered_map<std::string, TypeDescriptor> fields;
  std::unordered_map<std::string, FunctionDescriptor> methods;
};

struct SemanticModel final {
  std::unordered_map<std::string, TypeDescriptor> types;
  std::unordered_map<std::string, std::unordered_set<std::string>> enum_members;
  std::unordered_map<std::string, ClassDescriptor> classes;
  std::unordered_map<std::string, FunctionDescriptor> functions;
};

struct TypeCheckResult final {
  Diagnostics diagnostics;
  std::size_t function_count = 0;
  SemanticModel model;
};

class TypeChecker final {
 public:
  [[nodiscard]] auto Check(const Program& program) -> TypeCheckResult;

 private:
  using Type = TypeDescriptor;
  using FunctionInfo = FunctionDescriptor;
  using ClassInfo = ClassDescriptor;

  [[nodiscard]] static auto InvalidType() -> Type;
  void AddDiagnostic(SourceLocation location, std::string message);
  void CollectTypes(const Program& program);
  void ResolveDeclarations(const Program& program);
  void CheckFunction(const Function& function, const ClassInfo* owner);
  void CheckStatement(const Statement& statement, const Type& return_type);
  auto CheckExpression(const Expression& expression, const Type* expected) -> Type;
  auto CheckCall(const Expression& expression, const Type* expected) -> Type;
  auto CheckFunctionCall(const FunctionInfo& function,
                         const std::vector<CallArgument>& arguments,
                         SourceLocation location) -> Type;
  auto CheckClassConstruction(const ClassInfo& class_info,
                              const std::vector<CallArgument>& arguments,
                              SourceLocation location) -> Type;
  auto CheckMemberAccess(const Expression& expression) -> Type;
  auto ResolveType(const TypeName& type_name, bool allow_void) -> Type;
  auto LookupName(const Expression& expression) -> Type;
  [[nodiscard]] auto LookupLocal(std::string_view name) const -> Type;
  void BeginScope();
  void EndScope();
  auto DeclareLocal(std::string_view name, const Type& type, SourceLocation location) -> bool;
  [[nodiscard]] static auto IsAssignable(const Expression& expression) -> bool;
  [[nodiscard]] static auto IsSameType(const Type& left, const Type& right) -> bool;
  [[nodiscard]] static auto IsNumeric(const Type& type) -> bool;
  [[nodiscard]] static auto IsValid(const Type& type) -> bool;
  [[nodiscard]] static auto TypeNameFor(const Type& type) -> std::string;

  void CheckVariableDeclaration(const Statement& statement);

  void CheckReturn(const Statement& statement, const Type& return_type);

  void CheckIf(const Statement& statement, const Type& return_type);

  void CheckWhile(const Statement& statement, const Type& return_type);

  void CheckFor(const Statement& statement, const Type& return_type);

  auto CheckIntegerLiteral(const Expression& expression) -> Type;

  auto CheckIndex(const Expression& expression) -> Type;

  auto CheckShorthandMember(const Expression& expression, const Type* expected) -> Type;

  auto CheckUnary(const Expression& expression) -> Type;

  auto CheckBinary(const Expression& expression) -> Type;

  auto ResolveSignature(const Function& function) -> FunctionInfo;
  auto CheckMethodCall(const Expression& expression, const Type* expected) -> Type;
  auto CheckAssignment(const Expression& expression) -> Type;

  Diagnostics diagnostics_;
  std::size_t function_count_ = 0;
  std::unordered_map<std::string, Type> types_;
  std::unordered_map<std::string, std::unordered_set<std::string>> enum_members_;
  std::unordered_map<std::string, ClassInfo> classes_;
  std::unordered_map<std::string, FunctionInfo> functions_;
  std::vector<std::unordered_map<std::string, Type>> scopes_;
  std::size_t loop_depth_ = 0;
};

}  // namespace vela::frontend
