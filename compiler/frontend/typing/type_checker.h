#pragma once

#include <cstdint>

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>

#include "base/array_algorithm.h"
#include "base/builtin.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/flow/control_flow.h"
#include "compiler/frontend/generics/specialization.h"
#include "compiler/frontend/modules/source_file.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_inference.h"

namespace vela::frontend {

struct TypeCheckResult final {
  Diagnostics diagnostics;
  std::size_t function_count = 0;
  SemanticModel model;
  [[nodiscard]] bool Succeeded() const { return !HasErrors(diagnostics); }
};

class TypeChecker final : private InferenceTypeRelations {
 public:
  explicit TypeChecker(DiagnosticOptions options = {}) : options_(options) {}
  // 每次检查消费独立上下文，结果转移后不再复用内部状态。
  [[nodiscard]] TypeCheckResult Check(Program& program,
                                      std::span<const GenericSpecialization> retained = {}) &&;
  // 声明准备后允许开发会话先比较契约；specializer 在本次检查期间持续拥有全部特化。
  [[nodiscard]] TypeCheckResult CheckPrepared(Program& program,
                                              Specializer& specializer,
                                              std::span<const GenericSpecialization> retained) &&;

 private:
  DiagnosticOptions options_;
  Specializer* specializer_ = nullptr;
  std::size_t registered_functions_ = 0;
  std::size_t registered_classes_ = 0;
  std::size_t registered_interfaces_ = 0;
  std::size_t registered_methods_ = 0;
  std::size_t checked_functions_ = 0;
  std::size_t checked_classes_ = 0;
  std::size_t checked_methods_ = 0;
  std::uint32_t next_interface_index_ = 0;
  absl::flat_hash_set<std::string> merged_classes_;
  absl::flat_hash_set<std::string> visiting_classes_;
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
    [[nodiscard]] Refinements Snapshot() const;
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
  // 泛型参数登记与上界：参数类型按身份唯一，上界在所有声明登记后解析。
  struct TypeParameterInfo final {
    const TypeParameter* declaration = nullptr;
    TypeDescriptor type;
    std::optional<TypeDescriptor> bound;
    bool resolved = false;
  };
  void RegisterTypeParameters(std::span<const TypeParameter> parameters);
  void RegisterMethodTemplates(std::span<const Function> methods);
  void RegisterTemplateParameters(const Program& program);
  [[nodiscard]] const TypeParameterInfo* FindTypeParameter(const Type& type) const;
  const Type* ResolveParameterBound(const TypeParameterInfo& parameter);
  void ValidateTypeParameters(std::span<const TypeParameter> parameters);
  [[nodiscard]] Type ParameterCapability(const Type& type, SourceLocation location);
  [[nodiscard]] bool IsPotentiallyNullable(const Type& type) const;
  void CheckDefinitions(Program& program);
  void CheckTemplateFunction(const Function& function, const ClassInfo* owner);
  void CheckTemplateClass(const Class& source);
  // 类型应用的上界验证在声明关系建立后统一执行；推导结果在调用点立即验证。
  struct BoundCheck final {
    std::string owner;
    std::vector<TypeParameter> parameters;
    std::vector<Type> arguments;
    SourceLocation location;
  };
  void QueueBoundCheck(std::string owner,
                       std::span<const TypeParameter> parameters,
                       std::vector<Type> arguments,
                       SourceLocation location);
  bool CheckTypeArguments(const BoundCheck& check);
  struct PendingApplication final {
    std::string owner;
    std::vector<TypeParameter> parameters;
    std::vector<TypeName> arguments;
    SourceLocation location;
  };
  bool DrainBoundChecks();
  void RegisterApplications();
  void RegisterInterface(const Interface& source, bool abstract);
  void RegisterSpecializations();
  void CheckPendingSpecializations();
  Type ResolveClassInstantiation(const TypeName& type_name);
  void CheckClassImplementation(const Class& source);
  void CheckInterfaceImplementation(ClassInfo& owner, const std::string& name);
  void CheckClassBody(const Class& source, const ClassInfo& info);
  Type InstantiateGeneric(const Expression& callee, const Expression* call, const Type* expected);
  [[nodiscard]] TypeName DescribeType(const Type& type) const;
  struct InferenceSignature final {
    std::vector<TypeParameter> type_parameters;
    std::vector<Parameter> parameters;
    TypeName return_type;
    absl::flat_hash_set<std::string> optional_parameters;
  };
  struct InferredCall final {
    std::vector<TypeName> type_arguments;
    FunctionInfo signature;
    std::vector<std::optional<Type>> arguments;
  };
  FunctionInfo ResolveAppliedSignature(const InferenceSignature& source,
                                       std::span<const TypeName> arguments,
                                       const Expression& callee);
  std::optional<InferenceSignature> GenericSignature(const Expression& callee);
  InferenceSignature FunctionInferenceSignature(const Function& function);
  void NormalizeInferenceSignature(InferenceSignature& source);
  enum class MethodAccess : std::uint8_t { Instance, Static, Super };
  std::optional<Type> CheckMethodApplication(
      const absl::flat_hash_map<std::string, MethodTemplate>& methods,
      const Expression& member,
      const Expression* call,
      const Type* expected,
      MethodAccess access);
  Type CheckGenericMethod(const MethodTemplate& method,
                          const Expression& member,
                          const Expression* call,
                          const Type* expected,
                          MethodAccess access);
  std::expected<MemberReference, Diagnostic> MaterializeMethod(const MethodTemplate& method,
                                                               std::span<const TypeName> arguments,
                                                               SourceLocation location);
  void RegisterMethodApplications();
  void QueueInterfaceBinding(const ClassInfo& owner,
                             const std::string& interface,
                             const Function& method);
  void DrainInterfaceBindings();
  void RestoreMethodSpecializations(std::span<const GenericSpecialization> retained);
  TypeInference MakeInference(std::span<const TypeParameter> parameters);
  void DiagnoseInferenceFailure(const TypeInference::Failure& failure, SourceLocation location);
  std::vector<TypeName> ProjectSupertypes(const Projection& projection) override;
  [[nodiscard]] bool IsStaticSubtype(const TypeName& subtype, const TypeName& supertype) override;
  [[nodiscard]] bool CanBindType(const TypeName& destination, const TypeName& source) override;
  [[nodiscard]] bool IsInvariantType(const TypeName& type) const override;
  static void SetConstructorParameters(const Class& owner, InferenceSignature& source);
  std::optional<std::vector<TypeName>> InferFunctionValue(const InferenceSignature& source,
                                                          const Expression& callee,
                                                          const Type* expected);
  enum class CallbackArity : std::uint8_t { Exact, Prefix };
  enum class CallbackPresence : std::uint8_t { Required, Optional };
  enum class CallbackParameters : std::uint8_t { Assignable, ExceptionFilter };
  std::optional<InferredCall> InferInvocation(const InferenceSignature& source,
                                              const Expression& expression,
                                              const Type* expected,
                                              std::span<const TypeName> explicit_arguments = {},
                                              CallbackArity arity = CallbackArity::Exact);
  std::optional<TypeInference> CreateInference(const InferenceSignature& source,
                                               SourceLocation location,
                                               const Type* expected,
                                               std::span<const TypeName> explicit_arguments);
  Type InferFutureResult(const Expression& expression,
                         TypeInference& inference,
                         const Type* actual);
  std::optional<TypeInference> CreateFutureInference(const Expression& expression,
                                                     const Type* expected);
  Type ResolveInferenceContext(const TypeName& pattern, const TypeInference& inference);
  std::vector<std::optional<Type>> CheckInferenceArguments(
      std::span<const Expression* const> expressions,
      std::span<const TypeName> patterns,
      TypeInference& inference,
      CallbackArity arity);
  Type CheckInferenceArgument(const Expression& expression,
                              const TypeName& pattern,
                              const Type& context,
                              TypeInference& inference,
                              CallbackArity arity);
  Type InferConstruction(const Expression& expression,
                         const Type* expected,
                         std::string_view name,
                         std::span<const TypeParameter> parameters);
  [[nodiscard]] bool CanCheckExpression(const Expression& expression, const Type* expected);
  bool CanInferInvocation(const Expression& expression, const Type* expected);
  const MethodTemplate* InferenceMethod(const Expression& member);
  void PrepareInferenceReceiver(const Expression& expression);
  Type PrepareInferenceExpression(const Expression& expression);
  [[nodiscard]] std::optional<std::size_t> CallbackParameterCount(const Expression& expression);
  Type CheckCallback(const Expression& expression,
                     FunctionInfo context,
                     CallbackArity arity = CallbackArity::Exact,
                     CallbackPresence presence = CallbackPresence::Required,
                     CallbackParameters parameters = CallbackParameters::Assignable);
  void CheckCallbackContract(const Expression& expression,
                             const Type& actual,
                             const FunctionInfo& context,
                             CallbackArity arity,
                             CallbackParameters parameters);
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
  [[nodiscard]] bool IsMatchingGenericSignature(const Function& implementation,
                                                const Function& signature);
  static void MergeInheritedMembers(ClassInfo& info, const ClassInfo& base);
  void CheckOverrides(const ClassInfo& info);
  void CheckGenericOverrides(const ClassInfo& info, const ClassInfo* base);
  void MergeBaseClass(std::string_view name,
                      absl::flat_hash_set<std::string>& merged,
                      absl::flat_hash_set<std::string>& visiting);
  void CheckFunction(const Function& function, const ClassInfo* owner);
  void CheckFunctionBody(const Function& function,
                         const FunctionInfo& function_info,
                         const ClassInfo* owner);
  void CheckGeneratedFunction(std::size_t index);
  void DeclareFunctionParameters(const Function& function,
                                 const FunctionInfo& signature,
                                 BindingKind kind);
  void CheckFieldInitializers(const Class& owner, const ClassInfo& info);
  void CheckAccess(Visibility visibility, std::string_view owner, SourceLocation location);
  [[nodiscard]] const ClassInfo* StaticOwner(const Expression& member) const;
  StatementFlow CheckStatement(const Statement& statement, const Type& return_type);
  static StatementFlow MergeBranches(StatementFlow left, StatementFlow right);
  StatementFlow CheckBlock(const Statement& statement, const Type& return_type);
  StatementFlow CheckSequence(const Statement& statement, const Type& return_type);
  StatementFlow CheckScopedStatement(const Statement& statement, const Type& return_type);
  StatementFlow CheckTry(const Statement& statement, const Type& return_type);
  Type CheckArrayMethod(const Expression& expression, const Type& receiver, const Type* expected);
  Type CheckArrayAlgorithm(const Expression& expression,
                           const Type& receiver,
                           ArrayAlgorithm algorithm,
                           const Type* expected);
  Type CheckArrayAlgorithmResult(ArrayAlgorithm algorithm,
                                 const Type& result,
                                 const FunctionInfo& method,
                                 SourceLocation location);
  Type CheckExpression(const Expression& expression, const Type* expected);
  Type CheckExpressionType(const Expression& expression, const Type* expected);
  Type CheckLambda(const Expression& expression, const Type* expected);
  void InferLambdaReturn(const Statement& statement);
  Type CheckTypeOperation(const Expression& expression);
  enum class MatchPhase : std::uint8_t { Prepare, Complete };
  Type CheckMatch(const Expression& expression,
                  const Type* expected,
                  MatchPhase phase = MatchPhase::Complete);
  struct MatchCoverage final {
    absl::flat_hash_set<std::string> labels;
    bool null = false;
    bool values = false;
  };
  [[nodiscard]] static bool IsMatchComplete(const Type& selector, const MatchCoverage& coverage);
  Type CheckMatchPattern(const MatchArm& arm, const Type& selector, MatchCoverage& coverage);
  void CheckMatchConstant(const MatchArm& arm, const Type& base, MatchCoverage& coverage);
  void CheckMatchResult(const Expression& expression, const Type* expected, Type& result);
  struct PreparedMatchArm;
  Type EnterMatchArm(const Expression& selector_expression,
                     const MatchArm& arm,
                     const Type& selector,
                     MatchCoverage& coverage,
                     PreparedMatchArm* prepared);
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
  Type CheckFutureAggregation(const Expression& expression, const Type* expected);
  Type ProjectFutureAggregationContext(const Expression& expression, const Type* expected);
  Type ProjectFutureArrayElement(const Expression& expression, const Type& values);
  Type CheckFutureComputation(const Expression& expression, const Type* expected);
  Type CheckFutureValueFactory(const Expression& expression, const Type* expected);
  Type CheckAsyncConstruction(const Expression& expression, const Type* expected);
  Type CheckAsyncMethod(const Expression& expression, const Type& receiver);
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
  Type CheckAsyncCallback(const Expression& expression,
                          std::span<const Type> parameters,
                          CallbackPresence presence = CallbackPresence::Required,
                          CallbackParameters contract = CallbackParameters::Assignable);
  Type RegisterAsyncCall(const Expression& expression,
                         AsyncCallDescriptor descriptor,
                         std::span<const std::optional<Type>> checked = {});
  Type CheckSleepCall(const Expression& expression);
  Type CheckIndirectCall(const Expression& expression, const Type& type);
  Type CheckDynamicCall(const Expression& expression);
  Type CheckStaticCall(const Expression& expression, const ClassInfo& owner, const Type* expected);
  Type CheckSuperMethodCall(const Expression& expression, const Type* expected);
  Type CheckSuperMember(const Expression& expression, const Type* expected);
  Type CheckClassMember(const Expression& expression, const ClassInfo& owner, const Type* expected);
  const ClassInfo* CheckSuperClass(SourceLocation location);
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
  Type CheckClassConstruction(const ClassInfo& class_info,
                              const Expression& expression,
                              std::span<const std::optional<Type>> checked_arguments = {});
  Type CheckMemberAccess(const Expression& expression, const Type* expected);
  Type CheckStaticMember(const Expression& expression,
                         const ClassInfo& owner,
                         const Type* expected);
  Type CheckObjectMember(const Expression& expression, const Type& object, const Type* expected);
  Type CheckBuiltinMethodValue(const Expression& expression,
                               const Type& receiver,
                               const Type* expected);
  std::optional<FunctionInfo> InferAsyncMethod(const Type& receiver, std::string_view name);
  std::optional<FunctionInfo> InferBuiltinMethod(const Type& receiver, std::string_view name);
  Type GenerateBuiltinMethod(const Expression& expression,
                             const Type& receiver,
                             const FunctionInfo& signature);
  enum class VoidTypePolicy : std::uint8_t { Allowed, Forbidden };
  Type ResolveType(const TypeName& type_name, VoidTypePolicy void_type);
  Type ResolveNamedType(const TypeName& type_name);
  Type ResolveFunctionType(const TypeName& type_name);
  Type ArrayType(const Type& element);
  Type LookupName(const Expression& expression, const Type* expected);
  [[nodiscard]] const LocalBinding* LookupLocal(std::string_view name) const;
  void BeginScope();
  void EndScope();
  enum class CheckStatus : std::uint8_t { Succeeded, Failed };
  CheckStatus DeclareLocal(std::string_view name,
                           const Type& type,
                           SourceLocation location,
                           BindingKind kind = BindingKind::Mutable,
                           const void* declaration = nullptr);
  [[nodiscard]] static bool IsAssignable(const Expression& expression);
  [[nodiscard]] static bool IsSameType(const Type& left, const Type& right);
  // 静态可证明的类型关系：身份、可空提升、class 继承、显式 implements、参数上界与 Future 载荷契约。
  // 不含 dynamic 边界；约束证明只使用此关系，普通赋值另外接受受检查的 dynamic 转换。
  [[nodiscard]] bool IsSubtype(const Type& subtype, const Type& supertype) const;
  [[nodiscard]] std::vector<Type> DirectSupertypes(const Type& type) const;
  [[nodiscard]] bool CanAssign(const Type& target, const Type& value) const;
  void RecordConversion(const Expression& expression, const Type& target, const Type& actual);
  void DiagnoseDynamicConversion(const Expression& expression,
                                 const Type& target,
                                 const Type& actual);
  [[nodiscard]] static Type NonNullable(Type type);
  void RefineCondition(const Expression& condition, ConditionTruth outcome);
  void RefineTypeTest(const Expression& condition);
  void RefineAssignedLocal(const LocalBinding& binding, const Type& value);
  void DiagnoseOperandMismatch(const Expression& operand,
                               const Type& type,
                               std::string_view message);
  void DiagnoseBinaryOperandMismatch(const Expression& expression,
                                     const Type& left,
                                     const Type& right,
                                     std::string_view message);
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
  // 诊断中的类型文本；类型参数显示源码名字，不暴露规范身份。
  [[nodiscard]] std::string TypeNameFor(const Type& type) const;

  void CheckVariableDeclaration(const Statement& statement);

  void CheckReturn(const Statement& statement, const Type& return_type);
  void CheckVoidReturn(const Statement& statement, const Type& return_type);

  StatementFlow CheckIf(const Statement& statement, const Type& return_type);

  StatementFlow CheckWhile(const Statement& statement, const Type& return_type);
  StatementFlow CheckLoopExit(const Statement& statement,
                              ConditionTruth condition,
                              StatementFlow body);

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
  Type CheckMapLiteral(const Expression& expression, const Type* expected);

  Type CheckUnary(const Expression& expression);
  Type CheckConditional(const Expression& expression, const Type* expected);

  Type CheckBinary(const Expression& expression, const Type* expected);
  Type CheckBinaryTypes(const Expression& expression, const Type& left, const Type& right);
  Type CheckBoundBinaryTypes(const Expression& expression, const Type& left, const Type& right);
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
  CheckStatus CheckAssignmentTarget(const Expression& target);
  Type CheckCompoundAssignment(const Expression& expression);
  Type CheckCompoundAssignmentTypes(const Expression& expression,
                                    const Type& left,
                                    const Type& right);
  Type CheckAwait(const Expression& expression, const Type* expected);

  Diagnostics diagnostics_;
  absl::flat_hash_set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::string>>
      dynamic_diagnostics_;
  absl::flat_hash_map<std::string, Type> types_;
  absl::flat_hash_map<std::string, Type> array_elements_;
  absl::node_hash_map<std::string, FunctionInfo> function_types_;
  absl::flat_hash_map<std::string, const Program::TypeAlias*> aliases_;
  absl::flat_hash_set<std::string> resolving_aliases_;
  struct LambdaScope final {
    std::size_t index = 0;
    std::size_t scope = 0;
    std::optional<Type> inferred_return = std::nullopt;
  };
  std::vector<LambdaScope> lambda_scopes_;
  absl::flat_hash_set<std::uint32_t> captured_mutables_;
  absl::flat_hash_set<const void*> captured_declarations_;
  struct PreparedExpression final {
    Type type;
    RefinementDelta refinements;
  };
  absl::flat_hash_map<const Expression*, PreparedExpression> prepared_expressions_;
  struct PreparedMatchArm final {
    Type narrowed;
    absl::flat_hash_map<std::string, LocalBinding> scope;
  };
  absl::flat_hash_map<const Expression*, std::vector<PreparedMatchArm>> prepared_matches_;
  absl::flat_hash_map<std::uint32_t, ConstantValue> constant_bindings_;
  absl::flat_hash_map<std::string, std::int64_t> enum_values_;
  absl::flat_hash_map<std::string, std::size_t> builtin_function_ids_;
  absl::node_hash_map<std::string, ClassInfo> classes_;
  absl::node_hash_map<std::string, FunctionInfo> functions_;
  std::vector<absl::flat_hash_map<std::string, LocalBinding>> scopes_;
  std::size_t loop_depth_ = 0;
  std::size_t switch_depth_ = 0;
  std::uint32_t next_binding_id_ = 0;
  Refinements refinements_;
  std::span<const SourceFile> sources_;
  const ClassInfo* owner_ = nullptr;
  bool in_constructor_ = false;
  bool in_async_function_ = false;
  bool checking_definitions_ = false;
  struct InterfaceBinding final {
    std::string owner;
    std::string interface;
    std::string method;
    SourceLocation location;
  };
  std::vector<InterfaceBinding> interface_bindings_;
  absl::flat_hash_set<std::tuple<std::string, std::string, std::string>> queued_interface_bindings_;
  absl::flat_hash_map<std::string, std::vector<std::string>> interface_implementers_;
  absl::flat_hash_set<std::pair<std::string, std::string>> registered_implementers_;
  absl::flat_hash_map<std::string, GlobalDescriptor> globals_;
  absl::flat_hash_set<std::string> checking_globals_;
  absl::flat_hash_set<std::string> checked_globals_;
  absl::node_hash_map<std::string, InterfaceDescriptor> interfaces_;
  absl::flat_hash_set<std::string> explicit_super_constructors_;
  absl::flat_hash_map<std::string, std::vector<TypeDescriptor>> generic_arguments_;
  absl::flat_hash_map<std::string, std::size_t> enum_sizes_;
  // 解析上界可能登记新的参数；节点容器保持已有条目地址稳定。
  absl::node_hash_map<TypeParameterId, TypeParameterInfo> type_parameters_;
  std::vector<BoundCheck> bound_checks_;
  std::vector<PendingApplication> pending_applications_;
  absl::flat_hash_set<std::string> checked_applications_;
  // 定义检查失败的模板不再检查其具体特化的函数体，避免同一错误以不同形式重复报告。
  absl::flat_hash_set<const void*> failed_templates_;
  NodeFacts facts_;
  NodeFacts definitions_;
  absl::flat_hash_map<std::string, JSONDecodeRequest> json_decode_requests_;
};

}  // namespace vela::frontend
