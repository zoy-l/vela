#pragma once

#include <cstdint>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/array_algorithm.h"
#include "base/async_operation.h"
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
    Dynamic,
    Bool,
    Int,
    Double,
    String,
    MutableString,
    ByteArray,
    ByteData,
    RegExp,
    RegExpMatch,
    Array,
    Function,
    Enum,
    Class,
    Interface,
    Future,
    Completer,
    Timer,
    CancellationToken,
    Set,
    Map,
  };

  Kind kind = Kind::Invalid;
  std::string name;
  bool nullable = false;
};

[[nodiscard]] TypeDescriptor MakeArrayType(const TypeDescriptor& element);
[[nodiscard]] TypeDescriptor MakeNullableType(TypeDescriptor type);

struct FunctionDescriptor final {
  struct Parameter final {
    std::string name;
    TypeDescriptor type;
    bool rest = false;
  };
  TypeDescriptor return_type;
  std::vector<Parameter> parameters;
  absl::flat_hash_map<std::string, std::uint32_t> parameter_indices;
  absl::flat_hash_set<std::string> optional_parameters;
  std::optional<Builtin> intrinsic;
  Visibility visibility = Visibility::Public;
  MemberStorage storage = MemberStorage::Instance;
  bool is_getter = false;
  bool is_setter = false;
  std::optional<TypeDescriptor> setter_parameter_type;
  std::optional<TypeDescriptor> setter_return_type;
};

[[nodiscard]] TypeDescriptor MakeFunctionType(const FunctionDescriptor& signature);

struct GeneratedFunction final {
  std::unique_ptr<Function> declaration;
  FunctionDescriptor signature;
};

struct AsyncCallDescriptor final {
  AsyncOperation operation;
  FunctionDescriptor signature;
  bool receiver = false;
  std::optional<TypeDescriptor> runtime_type = std::nullopt;
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
  std::optional<std::string> base_class;
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
  absl::flat_hash_map<const Expression*, TypeDescriptor> checked_conversions;
  absl::flat_hash_map<const Expression*, ConstantValue> case_values;
  absl::flat_hash_map<std::string, std::int64_t> enum_values;
  absl::flat_hash_map<const Expression*, Builtin> builtin_calls;
  absl::flat_hash_map<const Expression*, AsyncCallDescriptor> async_calls;
  std::vector<GeneratedFunction> generated_functions;
  absl::flat_hash_map<const Expression*, std::size_t> builtin_method_values;
  absl::flat_hash_map<const Expression*, ArrayAlgorithm> array_algorithms;
  absl::flat_hash_map<const Statement*, TypeDescriptor> local_types;
  absl::flat_hash_map<const Statement*, StatementFlow> control_flow;
  absl::flat_hash_map<std::string, ClassDescriptor> classes;
  absl::flat_hash_map<std::string, FunctionDescriptor> functions;
  absl::flat_hash_map<const Expression*, MemberReference> static_members;
  absl::flat_hash_map<std::string, InterfaceDescriptor> interfaces;
  absl::flat_hash_map<const Expression*, MemberReference> interface_members;
  absl::flat_hash_map<const Expression*, MemberReference> super_members;
  absl::flat_hash_set<std::string> explicit_super_constructors;
  absl::flat_hash_map<const Expression*, TypeDescriptor> type_operands;
  absl::flat_hash_map<std::string, std::vector<TypeDescriptor>> generic_arguments;
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
  struct BuiltinMethod final {
    Builtin operation = Builtin::Count;
    FunctionInfo signature;
  };
  struct LocalBinding final {
    Type type;
    BindingKind kind = BindingKind::Mutable;
    std::uint32_t id = 0;
    std::size_t scope = 0;
  };
  using RefinementDelta = absl::flat_hash_map<std::uint32_t, std::optional<Type>>;
  class Refinements final {
   public:
    struct Checkpoint final {
      std::size_t offset;
    };
    Refinements() = default;
    ~Refinements() = default;
    Refinements(const Refinements&) = delete;
    Refinements& operator=(const Refinements&) = delete;
    Refinements(Refinements&& other) noexcept;
    Refinements& operator=(Refinements&& other) noexcept;

    [[nodiscard]] const Type* Find(std::uint32_t binding) const;
    [[nodiscard]] bool IsEmpty() const;
    void Set(std::uint32_t binding, Type type);
    void Erase(std::uint32_t binding);
    void Erase(const absl::flat_hash_set<std::uint32_t>& bindings);
    void Clear();
    [[nodiscard]] Checkpoint BeginBranch();
    void Discard(Checkpoint checkpoint);
    [[nodiscard]] RefinementDelta Capture(Checkpoint checkpoint);
    void Apply(const RefinementDelta& changes);
    void Intersect(RefinementDelta& target, const RefinementDelta& other) const;

   private:
    struct Change final {
      std::uint32_t binding{};
      std::optional<Type> previous;
    };
    void Save(std::uint32_t binding, const Type* previous);
    absl::flat_hash_map<std::uint32_t, Type> facts_;
    std::vector<Change> undo_;
    std::size_t branch_depth_ = 0;
  };

  [[nodiscard]] static Type InvalidType();
  void AddDiagnostic(SourceLocation location, std::string message);
  void CheckBooleanOperand(const Expression& expression,
                           const Type& type,
                           std::string_view message);
  void CollectTypes(const Program& program);
  void ResolveDeclarations(const Program& program);
  void ResolveGlobals(const Program& program);
  Type ResolveGlobal(const std::string& name);
  Type CheckGlobalInitializer(const Global& declaration, const Type* expected);
  void ResolveClass(const Class& declaration, ClassInfo& info);
  void ResolveClassRelations(const Class& declaration, ClassInfo& info);
  void ResolveClassMembers(const Class& declaration, ClassInfo& info);
  void ResolveClassMethods(const Class& declaration, ClassInfo& info);
  void ResolveClassMethod(const Function& method, ClassInfo& info);
  void MergeAccessor(FunctionInfo& existing, const FunctionInfo& incoming, SourceLocation location);
  void CheckAccessor(const Function& method);
  void ResolveClassConstructor(const Class& declaration, ClassInfo& info);
  void ResolveField(const Field& field, const Class& declaration, ClassInfo& info);
  void ResolveInterfaces(const Program& program);
  void CheckImplementations(const Program& program);
  [[nodiscard]] static bool MatchesImplementationSignature(const FunctionInfo& implementation,
                                                           const FunctionInfo& signature);
  static void MergeInheritedMembers(ClassInfo& info, const ClassInfo& base);
  void CheckOverrides(const ClassInfo& info);
  void MergeBaseClass(std::string_view name,
                      absl::flat_hash_set<std::string>& merged,
                      absl::flat_hash_set<std::string>& visiting);
  void CheckFunction(const Function& function, const ClassInfo* owner);
  void CheckGeneratedFunction(std::size_t index);
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
  Type CheckArrayCallback(const Expression& expression, const FunctionInfo& expected);
  Type CheckArrayAlgorithmResult(ArrayAlgorithm algorithm,
                                 const Type& result,
                                 const FunctionInfo& method,
                                 SourceLocation location);
  Type CheckExpression(const Expression& expression, const Type* expected);
  Type CheckExpressionType(const Expression& expression, const Type* expected);
  Type CheckLambda(const Expression& expression, const Type* expected);
  void InferLambdaReturn(const Statement& statement);
  Type CheckTypeOperation(const Expression& expression);
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
  std::optional<BuiltinMethod> ResolveCollectionMethod(const Type& receiver, std::string_view name);
  Type ResolveGenericType(const TypeName& source);
  Type NullableType(Type type);
  Type FunctionType(const FunctionInfo& signature);
  void CaptureLocal(const Expression& expression, const LocalBinding& binding);
  Type CheckCall(const Expression& expression, const Type* expected);
  Type CheckNamedCall(const Expression& expression, const Type* expected);
  Type CheckFutureCall(const Expression& expression, const Type* expected);
  Type MakeFutureType(const Expression& expression, const Type& element);
  Type FutureAggregationElement(const Expression& expression, const Type* expected);
  Type CheckFutureAggregation(const Expression& expression, const Type* expected);
  Type CheckFutureComputation(const Expression& expression, const Type* expected);
  Type CheckFutureValueFactory(const Expression& expression, const Type* expected);
  Type CheckAsyncConstruction(const Expression& expression, const Type* expected);
  Type CheckAsyncMethod(const Expression& expression, const Type& receiver, const Type* expected);
  Type CheckCompleterMethod(const Expression& expression, const Type& receiver);
  Type CheckFutureCallback(const Expression& expression, const Type& receiver);
  Type CheckFutureTimeout(const Expression& expression, const Type& receiver);
  Type AddThenError(const Expression& expression,
                    FunctionInfo& signature,
                    std::vector<std::optional<Type>>& checked);
  void AddFutureCleanup(const Expression& expression,
                        FunctionInfo& signature,
                        const Type& element,
                        std::vector<std::optional<Type>>& checked);
  Type CheckTimerFactory(const Expression& expression);
  Type CheckAsyncProperty(const Expression& expression, const Type& receiver);
  enum class CallbackPresence : std::uint8_t { Required, Optional };
  Type CheckAsyncCallback(const Expression& expression,
                          std::span<const Type> parameters,
                          CallbackPresence presence = CallbackPresence::Required);
  Type RegisterAsyncCall(const Expression& expression,
                         AsyncCallDescriptor descriptor,
                         std::span<const std::optional<Type>> checked = {});
  Type CheckSleepCall(const Expression& expression);
  Type CheckIndirectCall(const Expression& expression, const Type& type);
  Type CheckDynamicCall(const Expression& expression);
  Type CheckStaticCall(const Expression& expression, const ClassInfo& owner);
  Type CheckSuperMethodCall(const Expression& expression);
  Type CheckMethodReceiver(const Expression& expression,
                           const Type& receiver,
                           const Type* expected);
  Type CheckInstanceCall(const Expression& expression, const Type& object, const Type* expected);
  void CheckMemberAssignment(const Expression& member);
  Type CheckFunctionCall(const FunctionInfo& function,
                         const Expression& expression,
                         std::span<const std::optional<Type>> checked_arguments = {});
  std::optional<std::size_t> CallParameterIndex(const FunctionInfo& function,
                                                const CallArgument& argument,
                                                std::size_t positional);
  Type CheckClassConstruction(const ClassInfo& class_info, const Expression& expression);
  Type CheckMemberAccess(const Expression& expression, const Type* expected);
  Type CheckStaticMember(const Expression& expression, const ClassInfo& owner);
  Type CheckObjectMember(const Expression& expression, const Type& object, const Type* expected);
  Type CheckBuiltinMethodValue(const Expression& expression,
                               const Type& receiver,
                               const Type* expected);
  std::optional<FunctionInfo> InferAsyncMethod(const Type& receiver, std::string_view name);
  std::optional<FunctionInfo> InferBuiltinMethod(const Type& receiver, std::string_view name);
  Type GenerateBuiltinMethod(const Expression& expression,
                             const Type& receiver,
                             const FunctionInfo& signature);
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
  void RecordConversion(const Expression& expression, const Type& target, const Type& actual);
  [[nodiscard]] static Type NonNullable(Type type);
  void RefineCondition(const Expression& condition, ConditionTruth outcome);
  void InvalidateAssignments(const Statement& statement);
  void InvalidateAssignments(const Expression& expression);
  struct AssignmentEffects final {
    absl::flat_hash_set<std::string_view> names;
    bool may_call = false;
  };
  class LoopAssignments final {
   public:
    explicit LoopAssignments(const Statement& statement) : statement_(&statement) {}
    const AssignmentEffects& Body();
    const AssignmentEffects& Whole();

   private:
    const Statement* statement_;
    std::optional<AssignmentEffects> body_;
    std::optional<AssignmentEffects> whole_;
  };
  static void CollectAssignments(const Statement& statement, AssignmentEffects& effects);
  static void CollectAssignments(const Expression& expression, AssignmentEffects& effects);
  void InvalidateAssignments(const AssignmentEffects& effects);
  void InvalidateCapturedRefinements();
  [[nodiscard]] static bool IsNumeric(const Type& type);
  [[nodiscard]] static bool IsValid(const Type& type);
  [[nodiscard]] static std::string TypeNameFor(const Type& type);

  void CheckVariableDeclaration(const Statement& statement);

  void CheckReturn(const Statement& statement, const Type& return_type);
  void CheckVoidReturn(const Statement& statement, const Type& return_type);

  StatementFlow CheckIf(const Statement& statement, const Type& return_type);

  StatementFlow CheckWhile(const Statement& statement, const Type& return_type);

  StatementFlow CheckFor(const Statement& statement, const Type& return_type);
  StatementFlow CheckForEach(const Statement& statement, const Type& return_type);
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
  Type CheckConditional(const Expression& expression);

  Type CheckBinary(const Expression& expression);
  Type CheckBinaryTypes(const Expression& expression, const Type& left, const Type& right);
  Type CheckLogicalTypes(const Expression& expression, const Type& left, const Type& right);
  Type CheckCoalesceTypes(const Expression& expression, const Type& left, const Type& right);
  Type CheckBitwiseTypes(const Expression& expression, const Type& left, const Type& right);
  Type CheckRelationalTypes(const Expression& expression, const Type& left, const Type& right);
  Type CheckEquality(const Expression& expression, const Type& left, const Type& right);

  FunctionInfo ResolveSignature(const Function& function, const FunctionInfo* context = nullptr);
  [[nodiscard]] bool CanFallThrough(const Function& function, const Type& return_type) const;
  Type CheckMethodCall(const Expression& expression, const Type* expected);
  Type CheckBuiltinCall(const Expression& expression, Builtin builtin);
  Type CheckAssignment(const Expression& expression);
  Type CheckCompoundAssignment(const Expression& expression);
  Type CheckCompoundAssignmentTypes(const Expression& expression,
                                    std::string_view operation,
                                    const Type& left,
                                    const Type& right);
  Type CheckAwait(const Expression& expression, const Type* expected);

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
  absl::flat_hash_map<const Expression*, Type> checked_conversions_;
  absl::flat_hash_map<const Expression*, ConstantValue> case_values_;
  absl::flat_hash_map<std::uint32_t, ConstantValue> constant_bindings_;
  absl::flat_hash_map<std::string, std::int64_t> enum_values_;
  absl::flat_hash_map<const Expression*, Builtin> builtin_calls_;
  absl::flat_hash_map<const Expression*, AsyncCallDescriptor> async_calls_;
  std::vector<GeneratedFunction> generated_functions_;
  absl::flat_hash_map<const Expression*, std::size_t> builtin_method_values_;
  absl::flat_hash_map<std::string, std::size_t> builtin_function_ids_;
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
  bool in_constructor_ = false;
  bool in_async_function_ = false;
  absl::flat_hash_map<const Expression*, MemberReference> static_members_;
  absl::flat_hash_map<std::string, GlobalDescriptor> globals_;
  absl::flat_hash_set<std::string> checking_globals_;
  absl::flat_hash_set<std::string> checked_globals_;
  absl::flat_hash_map<std::string, InterfaceDescriptor> interfaces_;
  absl::flat_hash_map<const Expression*, MemberReference> interface_members_;
  absl::flat_hash_map<const Expression*, MemberReference> super_members_;
  absl::flat_hash_set<std::string> explicit_super_constructors_;
  absl::flat_hash_map<const Expression*, TypeDescriptor> type_operands_;
  absl::flat_hash_map<std::string, std::vector<TypeDescriptor>> generic_arguments_;
  absl::flat_hash_map<const MatchArm*, TypeDescriptor> match_bindings_;
  absl::flat_hash_map<const Statement::Catch*, TypeDescriptor> catch_types_;
  absl::flat_hash_map<const Expression*, std::vector<std::uint32_t>> argument_order_;
  absl::flat_hash_set<const Expression*> collection_constructions_;
  absl::flat_hash_map<std::string, std::size_t> enum_sizes_;
};

}  // namespace vela::frontend
