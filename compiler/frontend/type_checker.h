#pragma once

#include <cstdint>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/control_flow.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_file.h"
#include "compiler/frontend/symbol_ids.h"

namespace vela::frontend {

using ConstantValue = std::variant<std::int64_t, bool, std::string>;

struct TypeDescriptor final {
  enum class Kind : std::uint8_t {
    Invalid,
    Null,
    Void,
    Bool,
    Int,
    Double,
    String,
    MutableString,
    Array,
    Function,
    Enum,
    Class,
    Interface,
    Expected,
    Set,
    Map,
  };

  Kind kind = Kind::Invalid;
  std::string name;
  bool nullable = false;
};

[[nodiscard]] TypeDescriptor MakeArrayType(const TypeDescriptor& element);

struct FunctionDescriptor final {
  struct Parameter final {
    std::string name;
    TypeDescriptor type;
  };
  TypeDescriptor return_type;
  std::vector<Parameter> parameters;
  absl::flat_hash_map<std::string, std::uint32_t> parameter_indices;
  absl::flat_hash_set<std::string> optional_parameters;
  Visibility visibility = Visibility::Public;
  MemberStorage storage = MemberStorage::Instance;
};

struct ClassDescriptor final {
  struct Field final {
    TypeDescriptor type;
    std::uint32_t index = 0;
    Visibility visibility = Visibility::Public;
  };
  TypeDescriptor type;
  std::vector<std::string> field_order;
  absl::flat_hash_map<std::string, Field> fields;
  absl::flat_hash_map<std::string, FunctionDescriptor> methods;
  FunctionDescriptor constructor;
  const Class* declaration = nullptr;
  absl::flat_hash_map<std::string, Field> static_fields;
  absl::flat_hash_set<std::string> interfaces;
};

struct InterfaceDescriptor final {
  TypeDescriptor type;
  absl::flat_hash_map<std::string, FunctionDescriptor> methods;
  std::uint32_t index = 0;
};

struct GlobalDescriptor final {
  TypeDescriptor type;
  const Global* declaration = nullptr;
};

struct Capture final {
  std::string name;
  TypeDescriptor type;
  std::uint32_t binding = 0;
  BindingKind kind = BindingKind::Constant;
};

struct LambdaDescriptor final {
  const Expression* expression = nullptr;
  FunctionDescriptor signature;
  std::vector<Capture> captures;
};

struct MemberReference final {
  std::string owner;
  std::string member;
};

enum class ExpectedOperation : std::uint8_t { Ok, Err, HasValue, Value, Error };
enum class ArrayAlgorithm : std::uint8_t {
  Sort,
  ForEach,
  Some,
  Every,
  FindIndex,
  Reduce,
  Map,
};

struct SemanticModel final {
  absl::flat_hash_map<std::string, GlobalDescriptor> globals;
  absl::flat_hash_map<std::string, TypeDescriptor> types;
  absl::flat_hash_map<std::string, TypeDescriptor> array_elements;
  absl::flat_hash_map<std::string, FunctionDescriptor> function_types;
  std::vector<LambdaDescriptor> lambdas;
  absl::flat_hash_map<const Expression*, std::size_t> lambda_indices;
  absl::flat_hash_map<const void*, std::uint32_t> bindings;
  absl::flat_hash_set<std::uint32_t> captured_mutables;
  // 借用受检 AST 的节点地址；模型只在该 AST 存活期间使用。
  absl::flat_hash_map<const Expression*, TypeDescriptor> expression_types;
  absl::flat_hash_map<const Expression*, ConstantValue> case_values;
  absl::flat_hash_map<std::string, std::int64_t> enum_values;
  absl::flat_hash_map<const Expression*, Builtin> builtin_calls;
  absl::flat_hash_map<const Expression*, ArrayAlgorithm> array_algorithms;
  absl::flat_hash_map<const Statement*, TypeDescriptor> local_types;
  absl::flat_hash_map<const Statement*, StatementFlow> control_flow;
  absl::flat_hash_map<std::string, ClassDescriptor> classes;
  absl::flat_hash_map<std::string, FunctionDescriptor> functions;
  absl::flat_hash_map<const Expression*, MemberReference> static_members;
  absl::flat_hash_map<std::string, InterfaceDescriptor> interfaces;
  absl::flat_hash_map<const Expression*, MemberReference> interface_members;
  absl::flat_hash_map<const Expression*, TypeDescriptor> type_operands;
  absl::flat_hash_map<std::string, std::vector<TypeDescriptor>> generic_arguments;
  absl::flat_hash_map<const Expression*, ExpectedOperation> expected_calls;
  absl::flat_hash_map<const MatchArm*, TypeDescriptor> match_bindings;
  absl::flat_hash_map<const Statement::Catch*, TypeDescriptor> catch_types;
  absl::flat_hash_map<const Expression*, std::vector<std::uint32_t>> argument_order;
  absl::flat_hash_set<const Expression*> collection_constructions;
};

struct TypeCheckResult final {
  Diagnostics diagnostics;
  std::size_t function_count = 0;
  SemanticModel model;
};

class TypeChecker final {
 public:
  explicit TypeChecker(SymbolIds* interface_ids = nullptr) : interface_ids_(interface_ids) {}
  [[nodiscard]] TypeCheckResult Check(const Program& program);

 private:
  SymbolIds* interface_ids_;
  using Type = TypeDescriptor;
  using FunctionInfo = FunctionDescriptor;
  using ClassInfo = ClassDescriptor;
  struct LocalBinding final {
    Type type;
    BindingKind kind = BindingKind::Mutable;
    std::uint32_t id = 0;
    std::size_t scope = 0;
  };
  using Refinements = absl::flat_hash_map<std::uint32_t, Type>;

  [[nodiscard]] static Type InvalidType();
  void AddDiagnostic(SourceLocation location, std::string message);
  void CollectTypes(const Program& program);
  void ResolveDeclarations(const Program& program);
  void ResolveGlobals(const Program& program);
  Type ResolveGlobal(const std::string& name);
  Type CheckGlobalInitializer(const Global& declaration, const Type* expected);
  void ResolveClass(const Class& declaration, ClassInfo& info);
  void ResolveField(const Field& field, const Class& declaration, ClassInfo& info);
  void ResolveInterfaces(const Program& program);
  void CheckImplementations(const Program& program);
  void CheckFunction(const Function& function, const ClassInfo* owner);
  void CheckFieldInitializers(const Class& owner, const ClassInfo& info);
  void CheckAccess(Visibility visibility, std::string_view owner, SourceLocation location);
  [[nodiscard]] const ClassInfo* StaticOwner(const Expression& member) const;
  StatementFlow CheckStatement(const Statement& statement, const Type& return_type);
  StatementFlow CheckBlock(const Statement& statement, const Type& return_type);
  StatementFlow CheckSequence(const Statement& statement, const Type& return_type);
  StatementFlow CheckScopedStatement(const Statement& statement, const Type& return_type);
  StatementFlow CheckTry(const Statement& statement, const Type& return_type);
  Type CheckArrayMethod(const Expression& expression, const Type& receiver);
  Type CheckArrayAlgorithm(const Expression& expression,
                           const Type& receiver,
                           ArrayAlgorithm algorithm);
  Type CheckArrayMap(const Expression& expression, const Type& receiver);
  Type CheckExpression(const Expression& expression, const Type* expected);
  Type CheckExpressionType(const Expression& expression, const Type* expected);
  Type CheckLambda(const Expression& expression, const Type* expected);
  void InferLambdaReturn(const Statement& statement);
  Type CheckTypeOperation(const Expression& expression);
  Type CheckExpectedFactory(const Expression& expression, const Type* expected);
  Type CheckExpectedMethod(const Expression& expression, const Type& receiver);
  Type CheckMatch(const Expression& expression, const Type* expected);
  struct MatchCoverage final {
    absl::flat_hash_set<std::string> labels;
    bool null = false;
    bool values = false;
  };
  [[nodiscard]] static bool IsMatchComplete(const Type& selector, const MatchCoverage& coverage);
  Type CheckMatchPattern(const MatchArm& arm, const Type& selector, MatchCoverage& coverage);
  void CheckMatchConstant(const MatchArm& arm, const Type& base, MatchCoverage& coverage);
  void CheckMatchResult(const Expression& expression, const Type* expected, Type& result);
  Type CheckCollectionConstruction(const Expression& expression, const Type* expected);
  Type CheckCollectionMethod(const Expression& expression, const Type& receiver);
  Type ResolveGenericType(const TypeName& source);
  Type NullableType(Type type);
  Type FunctionType(const FunctionInfo& signature);
  void CaptureLocal(const Expression& expression, const LocalBinding& binding);
  Type CheckCall(const Expression& expression, const Type* expected);
  Type CheckNamedCall(const Expression& expression, const Type* expected);
  Type CheckIndirectCall(const Expression& expression, const Type& type);
  Type CheckStaticCall(const Expression& expression, const ClassInfo& owner);
  Type CheckInstanceCall(const Expression& expression, const Type& object, const Type* expected);
  void CheckMemberAssignment(const Expression& member);
  Type CheckFunctionCall(const FunctionInfo& function,
                         const Expression& expression,
                         std::span<const std::optional<Type>> checked_arguments = {});
  Type CheckClassConstruction(const ClassInfo& class_info, const Expression& expression);
  Type CheckMemberAccess(const Expression& expression);
  Type CheckStaticMember(const Expression& expression, const ClassInfo& owner);
  Type CheckObjectMember(const Expression& expression, const Type& object);
  Type ResolveType(const TypeName& type_name, bool allow_void);
  Type ArrayType(const Type& element);
  Type LookupName(const Expression& expression);
  [[nodiscard]] const LocalBinding* LookupLocal(std::string_view name) const;
  void BeginScope();
  void EndScope();
  bool DeclareLocal(std::string_view name,
                    const Type& type,
                    SourceLocation location,
                    BindingKind kind = BindingKind::Mutable,
                    const void* declaration = nullptr);
  [[nodiscard]] static bool IsAssignable(const Expression& expression);
  [[nodiscard]] static bool IsSameType(const Type& left, const Type& right);
  [[nodiscard]] bool CanAssign(const Type& target, const Type& value) const;
  [[nodiscard]] static Type NonNullable(Type type);
  void RefineCondition(const Expression& condition, ConditionTruth outcome);
  void InvalidateAssignments(const Statement& statement);
  void InvalidateAssignments(const Expression& expression);
  void InvalidateCapturedRefinements();
  [[nodiscard]] static Refinements IntersectFacts(const Refinements& left,
                                                  const Refinements& right);
  [[nodiscard]] static bool IsNumeric(const Type& type);
  [[nodiscard]] static bool IsValid(const Type& type);
  [[nodiscard]] static std::string TypeNameFor(const Type& type);

  void CheckVariableDeclaration(const Statement& statement);

  void CheckReturn(const Statement& statement, const Type& return_type);

  StatementFlow CheckIf(const Statement& statement, const Type& return_type);

  StatementFlow CheckWhile(const Statement& statement, const Type& return_type);

  StatementFlow CheckFor(const Statement& statement, const Type& return_type);
  StatementFlow CheckSwitch(const Statement& statement, const Type& return_type);
  [[nodiscard]] std::optional<ConstantValue> EvaluateConstant(const Expression& expression) const;
  [[nodiscard]] std::optional<ConstantValue> EvaluateUnaryConstant(
      const Expression& expression) const;
  [[nodiscard]] std::optional<ConstantValue> EvaluateBinaryConstant(
      const Expression& expression) const;
  [[nodiscard]] static std::string ConstantKey(const ConstantValue& value);
  void CheckCaseLabel(const Expression& label,
                      const Type& selector_type,
                      absl::flat_hash_set<std::string>& labels);

  Type CheckIntegerLiteral(const Expression& expression);

  Type CheckIndex(const Expression& expression);
  Type CheckArrayLiteral(const Expression& expression, const Type* expected);

  Type CheckUnary(const Expression& expression);

  Type CheckBinary(const Expression& expression);
  Type CheckBinaryTypes(const Expression& expression, const Type& left, const Type& right);
  Type CheckEquality(const Expression& expression, const Type& left, const Type& right);

  FunctionInfo ResolveSignature(const Function& function, const FunctionInfo* context = nullptr);
  Type CheckMethodCall(const Expression& expression, const Type* expected);
  Type CheckBuiltinCall(const Expression& expression, Builtin builtin);
  Type CheckAssignment(const Expression& expression);

  Diagnostics diagnostics_;
  std::size_t function_count_ = 0;
  absl::flat_hash_map<std::string, Type> types_;
  absl::flat_hash_map<std::string, Type> array_elements_;
  absl::flat_hash_map<std::string, FunctionInfo> function_types_;
  absl::flat_hash_map<std::string, const Program::TypeAlias*> aliases_;
  absl::flat_hash_set<std::string> resolving_aliases_;
  std::vector<LambdaDescriptor> lambdas_;
  absl::flat_hash_map<const Expression*, std::size_t> lambda_indices_;
  struct LambdaScope final {
    std::size_t index = 0;
    std::size_t scope = 0;
    std::optional<Type> inferred_return = std::nullopt;
  };
  std::vector<LambdaScope> lambda_scopes_;
  absl::flat_hash_map<const void*, std::uint32_t> bindings_;
  absl::flat_hash_set<std::uint32_t> captured_mutables_;
  absl::flat_hash_set<const void*> captured_declarations_;
  absl::flat_hash_map<const Expression*, Type> expression_types_;
  absl::flat_hash_map<const Expression*, ConstantValue> case_values_;
  absl::flat_hash_map<std::uint32_t, ConstantValue> constant_bindings_;
  absl::flat_hash_map<std::string, std::int64_t> enum_values_;
  absl::flat_hash_map<const Expression*, Builtin> builtin_calls_;
  absl::flat_hash_map<const Expression*, ArrayAlgorithm> array_algorithms_;
  absl::flat_hash_map<const Statement*, Type> local_types_;
  absl::flat_hash_map<const Statement*, StatementFlow> control_flow_;
  absl::flat_hash_map<std::string, ClassInfo> classes_;
  absl::flat_hash_map<std::string, FunctionInfo> functions_;
  std::vector<absl::flat_hash_map<std::string, LocalBinding>> scopes_;
  std::size_t loop_depth_ = 0;
  std::size_t switch_depth_ = 0;
  std::uint32_t next_binding_id_ = 0;
  Refinements refinements_;
  std::span<const SourceFile> sources_;
  const ClassInfo* owner_ = nullptr;
  absl::flat_hash_map<const Expression*, MemberReference> static_members_;
  absl::flat_hash_map<std::string, GlobalDescriptor> globals_;
  absl::flat_hash_set<std::string> checking_globals_;
  absl::flat_hash_set<std::string> checked_globals_;
  absl::flat_hash_map<std::string, InterfaceDescriptor> interfaces_;
  absl::flat_hash_map<const Expression*, MemberReference> interface_members_;
  absl::flat_hash_map<const Expression*, TypeDescriptor> type_operands_;
  absl::flat_hash_map<std::string, std::vector<TypeDescriptor>> generic_arguments_;
  absl::flat_hash_map<const Expression*, ExpectedOperation> expected_calls_;
  absl::flat_hash_map<const MatchArm*, TypeDescriptor> match_bindings_;
  absl::flat_hash_map<const Statement::Catch*, TypeDescriptor> catch_types_;
  absl::flat_hash_map<const Expression*, std::vector<std::uint32_t>> argument_order_;
  absl::flat_hash_set<const Expression*> collection_constructions_;
  absl::flat_hash_map<std::string, std::size_t> enum_sizes_;
};

}  // namespace vela::frontend
