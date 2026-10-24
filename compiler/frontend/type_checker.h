#pragma once

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
  enum class Kind {
    Invalid,
    Void,
    Bool,
    Int,
    Float,
    String,
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
  [[nodiscard]] TypeCheckResult Check(const Program& program);

 private:
  using Type = TypeDescriptor;
  using FunctionInfo = FunctionDescriptor;
  using ClassInfo = ClassDescriptor;

  [[nodiscard]] static Type InvalidType();
  void AddDiagnostic(SourceLocation location, std::string message);
  void CollectTypes(const Program& program);
  void ResolveDeclarations(const Program& program);
  void CheckFunction(const Function& function, const ClassInfo* owner);
  void CheckStatement(const Statement& statement, Type return_type);
  Type CheckExpression(const Expression& expression, const Type* expected);
  Type CheckCall(const Expression& expression, const Type* expected);
  Type CheckFunctionCall(const FunctionInfo& function,
                         const std::vector<CallArgument>& arguments,
                         SourceLocation location);
  Type CheckClassConstruction(const ClassInfo& class_info,
                              const std::vector<CallArgument>& arguments,
                              SourceLocation location);
  Type CheckMemberAccess(const Expression& expression);
  Type ResolveType(const TypeName& type_name, bool allow_void);
  Type LookupName(const Expression& expression);
  Type LookupLocal(std::string_view name) const;
  void BeginScope();
  void EndScope();
  bool DeclareLocal(std::string_view name, Type type, SourceLocation location);
  [[nodiscard]] bool IsAssignable(const Expression& expression) const;
  [[nodiscard]] bool IsSameType(Type left, Type right) const;
  [[nodiscard]] bool IsNumeric(Type type) const;
  [[nodiscard]] bool IsValid(Type type) const;
  [[nodiscard]] std::string TypeNameFor(Type type) const;

  Diagnostics diagnostics_;
  std::size_t function_count_ = 0;
  std::unordered_map<std::string, Type> types_;
  std::unordered_map<std::string, std::unordered_set<std::string>> enum_members_;
  std::unordered_map<std::string, ClassInfo> classes_;
  std::unordered_map<std::string, FunctionInfo> functions_;
  std::vector<std::unordered_map<std::string, Type>> scopes_;
};

}  // namespace vela::frontend
