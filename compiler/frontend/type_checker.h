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
    StringBuilder,
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
  Any,
  Every,
  FindIndex,
  Reduce,
  Transform,
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
  [[nodiscard]] auto Check(const Program& program) -> TypeCheckResult;

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

  [[nodiscard]] static auto InvalidType() -> Type;
  void AddDiagnostic(SourceLocation location, std::string message);
  void CollectTypes(const Program& program);
  void ResolveDeclarations(const Program& program);
  void ResolveGlobals(const Program& program);
  auto ResolveGlobal(const std::string& name) -> Type;
  auto CheckGlobalInitializer(const Global& declaration, const Type* expected) -> Type;
  void ResolveClass(const Class& declaration, ClassInfo& info);
  void ResolveField(const Field& field, const Class& declaration, ClassInfo& info);
  void ResolveInterfaces(const Program& program);
  void CheckImplementations(const Program& program);
  void CheckFunction(const Function& function, const ClassInfo* owner);
  void CheckFieldInitializers(const Class& owner, const ClassInfo& info);
  void CheckAccess(Visibility visibility, std::string_view owner, SourceLocation location);
  [[nodiscard]] auto StaticOwner(const Expression& member) const -> const ClassInfo*;
  auto CheckStatement(const Statement& statement, const Type& return_type) -> StatementFlow;
  auto CheckBlock(const Statement& statement, const Type& return_type) -> StatementFlow;
  auto CheckSequence(const Statement& statement, const Type& return_type) -> StatementFlow;
  auto CheckScopedStatement(const Statement& statement, const Type& return_type) -> StatementFlow;
  auto CheckTry(const Statement& statement, const Type& return_type) -> StatementFlow;
  auto CheckArrayMethod(const Expression& expression, const Type& receiver) -> Type;
  auto CheckArrayAlgorithm(const Expression& expression,
                           const Type& receiver,
                           ArrayAlgorithm algorithm) -> Type;
  auto CheckExpression(const Expression& expression, const Type* expected) -> Type;
  auto CheckExpressionType(const Expression& expression, const Type* expected) -> Type;
  auto CheckLambda(const Expression& expression) -> Type;
  auto CheckTypeOperation(const Expression& expression) -> Type;
  auto CheckExpectedFactory(const Expression& expression, const Type* expected) -> Type;
  auto CheckExpectedMethod(const Expression& expression, const Type& receiver) -> Type;
  auto CheckMatch(const Expression& expression, const Type* expected) -> Type;
  struct MatchCoverage final {
    absl::flat_hash_set<std::string> labels;
    bool null = false;
    bool values = false;
  };
  [[nodiscard]] static auto IsMatchComplete(const Type& selector, const MatchCoverage& coverage)
      -> bool;
  auto CheckMatchPattern(const MatchArm& arm, const Type& selector, MatchCoverage& coverage)
      -> Type;
  void CheckMatchConstant(const MatchArm& arm, const Type& base, MatchCoverage& coverage);
  void CheckMatchResult(const Expression& expression, const Type* expected, Type& result);
  auto CheckCollectionConstruction(const Expression& expression, const Type* expected) -> Type;
  auto CheckCollectionMethod(const Expression& expression, const Type& receiver) -> Type;
  auto ResolveGenericType(const TypeName& source) -> Type;
  auto NullableType(Type type) -> Type;
  auto FunctionType(const FunctionInfo& signature) -> Type;
  void CaptureLocal(const Expression& expression, const LocalBinding& binding);
  auto CheckCall(const Expression& expression, const Type* expected) -> Type;
  auto CheckNamedCall(const Expression& expression, const Type* expected) -> Type;
  auto CheckIndirectCall(const Expression& expression, const Type& type) -> Type;
  auto CheckStaticCall(const Expression& expression, const ClassInfo& owner) -> Type;
  auto CheckInstanceCall(const Expression& expression, const Type& object, const Type* expected)
      -> Type;
  void CheckMemberAssignment(const Expression& member);
  auto CheckFunctionCall(const FunctionInfo& function,
                         const Expression& expression,
                         std::span<const Type> checked_arguments = {}) -> Type;
  auto CheckClassConstruction(const ClassInfo& class_info, const Expression& expression) -> Type;
  auto CheckMemberAccess(const Expression& expression) -> Type;
  auto CheckStaticMember(const Expression& expression, const ClassInfo& owner) -> Type;
  auto CheckObjectMember(const Expression& expression, const Type& object) -> Type;
  auto ResolveType(const TypeName& type_name, bool allow_void) -> Type;
  auto ArrayType(const Type& element) -> Type;
  auto LookupName(const Expression& expression) -> Type;
  [[nodiscard]] auto LookupLocal(std::string_view name) const -> const LocalBinding*;
  void BeginScope();
  void EndScope();
  auto DeclareLocal(std::string_view name,
                    const Type& type,
                    SourceLocation location,
                    BindingKind kind = BindingKind::Mutable,
                    const void* declaration = nullptr) -> bool;
  [[nodiscard]] static auto IsAssignable(const Expression& expression) -> bool;
  [[nodiscard]] static auto IsSameType(const Type& left, const Type& right) -> bool;
  [[nodiscard]] auto CanAssign(const Type& target, const Type& value) const -> bool;
  [[nodiscard]] static auto NonNullable(Type type) -> Type;
  void RefineCondition(const Expression& condition, ConditionTruth outcome);
  void InvalidateAssignments(const Statement& statement);
  void InvalidateAssignments(const Expression& expression);
  void InvalidateCapturedRefinements();
  [[nodiscard]] static auto IntersectFacts(const Refinements& left, const Refinements& right)
      -> Refinements;
  [[nodiscard]] static auto IsNumeric(const Type& type) -> bool;
  [[nodiscard]] static auto IsValid(const Type& type) -> bool;
  [[nodiscard]] static auto TypeNameFor(const Type& type) -> std::string;

  void CheckVariableDeclaration(const Statement& statement);

  void CheckReturn(const Statement& statement, const Type& return_type);

  auto CheckIf(const Statement& statement, const Type& return_type) -> StatementFlow;

  auto CheckWhile(const Statement& statement, const Type& return_type) -> StatementFlow;

  auto CheckFor(const Statement& statement, const Type& return_type) -> StatementFlow;
  auto CheckSwitch(const Statement& statement, const Type& return_type) -> StatementFlow;
  [[nodiscard]] auto EvaluateConstant(const Expression& expression) const
      -> std::optional<ConstantValue>;
  [[nodiscard]] auto EvaluateUnaryConstant(const Expression& expression) const
      -> std::optional<ConstantValue>;
  [[nodiscard]] auto EvaluateBinaryConstant(const Expression& expression) const
      -> std::optional<ConstantValue>;
  [[nodiscard]] static auto ConstantKey(const ConstantValue& value) -> std::string;
  void CheckCaseLabel(const Expression& label,
                      const Type& selector_type,
                      absl::flat_hash_set<std::string>& labels);

  auto CheckIntegerLiteral(const Expression& expression) -> Type;

  auto CheckIndex(const Expression& expression) -> Type;
  auto CheckArrayLiteral(const Expression& expression, const Type* expected) -> Type;

  auto CheckUnary(const Expression& expression) -> Type;

  auto CheckBinary(const Expression& expression) -> Type;
  auto CheckBinaryTypes(const Expression& expression, const Type& left, const Type& right) -> Type;
  auto CheckEquality(const Expression& expression, const Type& left, const Type& right) -> Type;

  auto ResolveSignature(const Function& function) -> FunctionInfo;
  auto CheckMethodCall(const Expression& expression, const Type* expected) -> Type;
  auto CheckBuiltinCall(const Expression& expression, Builtin builtin) -> Type;
  auto CheckAssignment(const Expression& expression) -> Type;

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
