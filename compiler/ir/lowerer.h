#pragma once

#include "base/array_algorithm.h"
#include "base/async_operation.h"
#include "base/builtin.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/member.h"
#include "base/native_program_mode.h"
#include "base/reflection.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/declaration_bindings.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

struct LoweringResult final {
  Program program;
  frontend::Diagnostics diagnostics;

  [[nodiscard]] bool Succeeded() const noexcept { return !frontend::HasErrors(diagnostics); }
};

class Lowerer final {
 public:
  Lowerer(const frontend::Program& source,
          const frontend::SemanticModel& model,
          const DeclarationBindings& declarations,
          NativeProgramMode native_mode) noexcept
      : source_(source), model_(model), declarations_(&declarations), native_mode_(native_mode) {}

  // lowering 状态借用固定的 AST 和语义模型；执行上下文不允许复制或迁移。
  ~Lowerer() = default;
  Lowerer(const Lowerer&) = delete;
  Lowerer& operator=(const Lowerer&) = delete;
  Lowerer(Lowerer&&) = delete;
  Lowerer& operator=(Lowerer&&) = delete;

  [[nodiscard]] LoweringResult Lower() &&;

 private:
  struct Local final {
    RegisterId reg = 0;
    frontend::TypeDescriptor type;
    bool cell = false;
  };

  struct LoweredValue final {
    RegisterId reg = 0;
    frontend::TypeDescriptor type;
  };

  struct FunctionValuePlan final {
    std::uint32_t target = 0;
    CallableMode mode = CallableMode::Plain;
    std::span<const frontend::Capture> captures;
    std::optional<LoweredValue> receiver;
    const frontend::FunctionDescriptor* interface_signature = nullptr;
    std::optional<DeclarationId> binding_receiver;
  };

  using ClassMap = absl::flat_hash_map<std::string, ClassId>;
  using FunctionMap = absl::flat_hash_map<std::string, std::uint32_t>;
  using LocalMap = absl::flat_hash_map<std::string, Local>;
  struct ControlTargets final {
    enum class Kind : std::uint8_t { Loop, Switch };
    Kind kind = Kind::Loop;
    std::vector<std::size_t> break_jumps;
    std::vector<std::size_t> continue_jumps;
    std::size_t finally_depth = 0;
  };
  struct Finally final {
    const frontend::Statement* body = nullptr;
    std::size_t scope_depth = 0;
    std::size_t control_depth = 0;
    std::optional<std::uint32_t> outer_handler;
  };
  enum class Completion : std::uint8_t { Continue, Terminated };

  void AddDiagnostic(frontend::SourceLocation location, std::string message);
  void BuildClassTable();
  [[nodiscard]] Class BuildClass(const frontend::Class& source_class,
                                 const frontend::ClassDescriptor& class_info);
  void BuildNativeClassContracts();
  void BuildInterfaceSelectors();
  void BuildFunctionTable();
  std::uint32_t AddFunctionId(const std::string& name);
  void AddGlobalId(const std::string& name, InitializerKind kind);
  std::uint32_t ConstructorTarget(const frontend::ClassDescriptor& owner);
  void StoreFunction(Function function);
  void RecordParameterValueTypes(Function& function);
  void BeginFunctionBody(Function& function, const frontend::Class* owner = nullptr);
  void LowerInitializer(std::string name,
                        const frontend::Expression& initializer,
                        const frontend::TypeDescriptor& type,
                        frontend::SourceLocation location);
  void LowerFieldDefaultInitializers(const frontend::Class& owner);
  [[nodiscard]] Function PrepareFunction(const frontend::Function& source,
                                         const frontend::FunctionDescriptor& function_info,
                                         const frontend::Class* owner,
                                         const frontend::LambdaDescriptor* lambda);
  void LowerFunction(const frontend::Function& source,
                     const frontend::FunctionDescriptor& function_info,
                     const frontend::Class* owner = nullptr,
                     const frontend::LambdaDescriptor* lambda = nullptr);
  void LowerIntrinsicFunction(Function function,
                              const frontend::Function& source,
                              const frontend::Class* owner,
                              Builtin builtin);
  RegisterId EmitReflectionCall(const frontend::FunctionDescriptor& signature,
                                RegisterId arguments,
                                frontend::SourceLocation location,
                                std::size_t count);
  RegisterId EmitIntrinsicOperationArguments(Builtin builtin,
                                             std::uint32_t operation,
                                             std::span<const std::uint32_t> prefix,
                                             RegisterId argument_start,
                                             frontend::SourceLocation location,
                                             std::size_t argument_count,
                                             const frontend::TypeDescriptor& return_type);
  std::optional<LoweredValue> LowerReflectedInvocation(const frontend::Expression& expression,
                                                       const frontend::FunctionDescriptor& function,
                                                       const LoweredValue& descriptor);
  void LowerFunctionParameters(const frontend::Function& source,
                               const frontend::FunctionDescriptor& function_info,
                               const frontend::Class* owner,
                               const frontend::LambdaDescriptor* lambda);
  [[nodiscard]] bool HasInstanceInitialization(const frontend::ClassDescriptor& owner);
  [[nodiscard]] std::uint32_t ConstructorMaskCount(const frontend::ClassDescriptor& owner) const;
  struct ConstructorFieldMask final {
    std::uint32_t index;
    std::uint64_t bit;
  };
  [[nodiscard]] static ConstructorFieldMask ConstructorMask(FieldId field);
  void LowerConstructorInitializers(const frontend::ClassDescriptor& owner);
  CallSignature& RegisterFunctionSignature(Function& function,
                                           const frontend::Function& source,
                                           const frontend::FunctionDescriptor& function_info);
  std::optional<LoweredValue> LowerSuperGetter(const frontend::Expression& expression);
  void LowerCaptures(const frontend::Function& source, const frontend::LambdaDescriptor& lambda);
  void LowerStatement(const frontend::Statement& statement);
  void LowerBlock(const frontend::Statement& statement);
  void LowerScopedStatement(const frontend::Statement& statement);
  void LowerSwitch(const frontend::Statement& statement);
  void LowerTry(const frontend::Statement& statement);
  void LowerReturn(const frontend::Statement& statement);
  [[nodiscard]] RegisterId LowerFuturePayload(const LoweredValue& value,
                                              const frontend::TypeDescriptor& future,
                                              frontend::SourceLocation location);
  void LowerControlExit(const frontend::Statement& statement);
  Completion LowerFinalizers(std::size_t depth);
  RegisterId LowerConstantLabel(const frontend::Expression& source);
  std::size_t LowerLabelMismatch(const LoweredValue& selector,
                                 RegisterId label,
                                 frontend::SourceLocation location);
  LoweredValue LowerMatch(const frontend::Expression& expression);

  [[nodiscard]] LoweredValue LowerExpression(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerExpressionValue(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerName(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerAwait(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerConversion(LoweredValue value,
                                             const frontend::TypeDescriptor& target,
                                             frontend::SourceLocation location);
  [[nodiscard]] LoweredValue SnapshotBefore(LoweredValue value, const frontend::Expression& later);
  [[nodiscard]] LoweredValue LowerAssignment(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerNameAssignment(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerMemberAssignment(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerIndexAssignment(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerConstruction(const frontend::Expression& expression,
                                               const frontend::TypeDescriptor& expected);
  [[nodiscard]] LoweredValue LowerConstructorCall(std::span<const frontend::CallArgument> arguments,
                                                  std::span<const std::uint32_t> argument_order,
                                                  frontend::SourceLocation location,
                                                  const frontend::ClassDescriptor& info,
                                                  RegisterId object,
                                                  std::span<const LoweredValue> evaluated = {});
  void EmitConstructorInitialization(const frontend::ClassDescriptor& info,
                                     RegisterId object,
                                     std::span<const RegisterId> arguments,
                                     frontend::SourceLocation location);
  void LowerJSONDecodePlans();
  void LowerExposedConstructors();
  void LowerReflection();
  void LowerAttributes();
  CallSignature DescribeCallSignature(const frontend::FunctionDescriptor& source);
  CallSignature::Parameter DescribeCallParameter(
      const frontend::FunctionDescriptor& source,
      const frontend::FunctionDescriptor::Parameter& parameter);
  reflection::Type DescribeReflectionType(const frontend::TypeDescriptor& type);
  std::uint32_t DescribeReflectionArgument(const frontend::TypeDescriptor& type);
  std::vector<std::uint32_t> DescribeReflectionArguments(
      std::span<const frontend::TypeDescriptor> arguments);
  reflection::Member DescribeReflectionMethod(
      const frontend::TypeDescriptor& owner,
      std::uint32_t declaring_type,
      const frontend::Function& source,
      const absl::node_hash_map<std::string, frontend::FunctionDescriptor>& methods);
  void DescribeReflectionClass(const frontend::Class& owner);
  void DescribeReflectionFields(const frontend::Class& owner,
                                const frontend::ClassDescriptor& descriptor,
                                const ClassDeclarations& identities,
                                std::uint32_t type);
  void DescribeReflectionConstructor(const frontend::Class& owner,
                                     const frontend::ClassDescriptor& descriptor,
                                     std::uint32_t type);
  void RegisterReflectionSelections();
  void DescribeReflectionInterface(const frontend::Interface& owner);
  void DescribeReflectionEnum(const frontend::Enum& owner);
  void LowerConstructorFactory(const frontend::ClassDescriptor& owner, const std::string& name);
  [[nodiscard]] Function PrepareConstructorFactory(const frontend::ClassDescriptor& owner,
                                                   const std::string& name);
  [[nodiscard]] RegisterId LowerHostConstructorFactory(const frontend::ClassDescriptor& owner);
  [[nodiscard]] LoweredValue LowerMember(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerObjectMember(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerGetter(const frontend::Expression& expression,
                                         const LoweredValue& object);
  [[nodiscard]] LoweredValue LowerIndex(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerCall(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerDynamicCall(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerDynamicMethodCall(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerDynamicMember(const frontend::Expression& expression,
                                                Builtin operation = Builtin::DynamicMemberGet);
  [[nodiscard]] LoweredValue LowerDynamicAssignment(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerDynamicCompoundAssignment(const frontend::Expression& expression,
                                                            Opcode operation);
  [[nodiscard]] std::pair<RegisterId, RegisterId> LowerDynamicArguments(
      const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerNamedCall(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerMethodCall(const frontend::Expression& expression);
  [[nodiscard]] LoweredValue LowerFutureCall(const frontend::Expression& expression);
  [[nodiscard]] std::uint32_t AddSyncFutureFunction(const frontend::TypeDescriptor& return_type,
                                                    const frontend::TypeDescriptor& callback_type);
  [[nodiscard]] FunctionValuePlan ResolveFunctionValue(const frontend::Expression& expression);
  LoweredValue LowerFunctionValue(const frontend::Expression& expression);
  LoweredValue LowerIndirectCall(const frontend::Expression& expression);
  LoweredValue LowerInterfaceCall(const frontend::Expression& expression);
  LoweredValue LowerTypeOperation(const frontend::Expression& expression);
  std::uint32_t RegisterRuntimeType(const frontend::TypeDescriptor& type);
  ClassId RegisterEnumType(const frontend::TypeDescriptor& type);
  void RegisterClassMembers(const frontend::ClassDescriptor& descriptor);
  void RegisterBuiltinMembers();
  void RegisterAsyncMember(const frontend::TypeDescriptor& receiver, std::string_view name);
  void RegisterArrayAlgorithmMember(const frontend::TypeDescriptor& receiver,
                                    std::string_view name,
                                    ArrayAlgorithm algorithm);
  Function CreateMemberFunction(const frontend::TypeDescriptor& receiver,
                                std::string_view name,
                                const frontend::FunctionDescriptor& signature);
  void BeginGeneratedBody(Function& function);
  void PublishMemberFunction(const frontend::TypeDescriptor& receiver,
                             std::string_view name,
                             MemberKind kind,
                             Function function);
  void RegisterClassProperty(std::uint32_t receiver,
                             const frontend::ClassDescriptor& owner,
                             const std::string& name);
  void RegisterBuiltinReceiver(const frontend::TypeDescriptor& receiver,
                               std::span<const std::string> names);
  void RegisterBuiltinReceiverMember(const frontend::TypeDescriptor& receiver,
                                     std::string_view name);
  void RegisterBuiltinMember(const frontend::TypeDescriptor& receiver,
                             std::string_view name,
                             Builtin builtin,
                             const frontend::FunctionDescriptor& signature,
                             MemberKind kind);
  LoweredValue ReadLocal(const Local& local, frontend::SourceLocation location);
  static frontend::Capture MakeReceiverCapture(const frontend::TypeDescriptor& type);
  ClassId CallableClass(std::uint32_t function_id,
                        CallableMode mode,
                        std::span<const frontend::Capture> captures,
                        const frontend::FunctionDescriptor* interface_signature = nullptr);

  [[nodiscard]] const frontend::ClassDescriptor* FindClass(std::string_view name) const;
  [[nodiscard]] const Local* FindLocal(std::string_view name) const;
  [[nodiscard]] RegisterId AllocateRegister(frontend::SourceLocation location);
  [[nodiscard]] RegisterId AllocateRegisterWindow(std::size_t count,
                                                  frontend::SourceLocation location);
  [[nodiscard]] static bool IsInteger(const frontend::TypeDescriptor& type);
  [[nodiscard]] bool IsDynamicExpression(const frontend::Expression& expression) const;
  void Emit(Instruction instruction);
  [[nodiscard]] std::optional<DeclarationId> ReceiverDeclaration(
      const frontend::TypeDescriptor& type) const;
  [[nodiscard]] std::optional<DeclarationId> FieldDeclaration(const frontend::TypeDescriptor& type,
                                                              FieldId index) const;
  void BindJump(std::size_t jump);
  enum class ValueTypeRecording : std::uint8_t { Infer, Declare };
  void RecordValueType(RegisterId reg,
                       const frontend::TypeDescriptor& type,
                       ValueTypeRecording recording = ValueTypeRecording::Infer);
  static constexpr auto kConstructorMaskBits = std::numeric_limits<std::uint64_t>::digits;
  void RecordInstructionValueType(const Instruction& instruction);
  void ResolveCallValueTypes();
  void PushScope();
  void PopScope();
  void DeclareLocal(std::string_view name,
                    RegisterId reg,
                    frontend::TypeDescriptor type,
                    frontend::SourceLocation location,
                    const void* declaration = nullptr);

  void LowerVariableDeclaration(const frontend::Statement& statement);

  void LowerIf(const frontend::Statement& statement);

  void BeginControl(ControlTargets::Kind kind);
  void BindContinues(std::uint32_t target);
  void FinishControl(std::optional<std::size_t> exit_jump);
  std::optional<std::size_t> LowerLoopCondition(const frontend::Statement& statement);
  std::size_t BeginNullSafe(RegisterId receiver, frontend::SourceLocation location);
  void FinishNullSafe(std::optional<std::size_t> null_jump,
                      RegisterId result,
                      frontend::SourceLocation location);
  void LowerWhile(const frontend::Statement& statement);

  void LowerFor(const frontend::Statement& statement);
  void LowerForEach(const frontend::Statement& statement);

  LoweredValue LowerIntegerLiteral(const frontend::Expression& expression);
  LoweredValue EmitDoubleConstant(double value, frontend::SourceLocation location);

  LoweredValue EmitStringConstant(std::string_view value, frontend::SourceLocation location);
  LoweredValue LowerStringInterpolation(const frontend::Expression& expression);
  LoweredValue LowerBuiltin(const frontend::Expression& expression);
  void EmitBuiltinCall(Builtin builtin,
                       RegisterId result,
                       std::span<const RegisterId> values,
                       frontend::SourceLocation location);
  void EmitBuiltinCall(Builtin builtin,
                       RegisterId result,
                       RegisterId base,
                       frontend::SourceLocation location);
  void EmitIndexRead(RegisterId result,
                     const LoweredValue& receiver,
                     RegisterId index,
                     frontend::SourceLocation location);
  void EmitIndexWrite(const LoweredValue& receiver,
                      RegisterId index,
                      RegisterId value,
                      frontend::SourceLocation location);
  void LowerBuiltinDefaults(Builtin builtin, RegisterId base, frontend::SourceLocation location);
  void EmitRegExpLayoutArgument(Builtin builtin,
                                RegisterId base,
                                frontend::TypeDescriptor type,
                                frontend::SourceLocation location);
  LoweredValue LowerAsyncCall(const frontend::Expression& expression);
  void LowerAsyncArguments(const frontend::Expression& expression,
                           AsyncOperation operation,
                           RegisterId base,
                           const std::optional<LoweredValue>& receiver);
  LoweredValue LowerSnapshotArray(const frontend::Expression& expression, Builtin count_builtin);
  [[nodiscard]] static std::optional<Builtin> SnapshotCountBuiltin(Builtin builtin);
  void EmitSnapshotArray(Builtin count_builtin,
                         RegisterId base,
                         const frontend::TypeDescriptor& type,
                         frontend::SourceLocation location);
  LoweredValue LowerCollectionConstruction(const frontend::Expression& expression);
  std::uint32_t RegisterCollectionType(frontend::TypeDescriptor type);

  LoweredValue LowerArrayLiteral(const frontend::Expression& expression);
  LoweredValue LowerMapLiteral(const frontend::Expression& expression);
  LoweredValue LowerArrayMutation(const frontend::Expression& expression);
  Builtin PrepareArrayMutation(Builtin builtin,
                               RegisterId base,
                               std::int64_t delete_count,
                               frontend::SourceLocation location);
  LoweredValue LowerArrayAlgorithm(const frontend::Expression& expression);
  RegisterId EmitArrayCallback(RegisterId callback,
                               std::span<const RegisterId> arguments,
                               const frontend::FunctionDescriptor& signature,
                               std::span<const frontend::TypeDescriptor> argument_types,
                               RegisterId guard,
                               frontend::SourceLocation location);
  struct ArrayAlgorithmRegisters final {
    RegisterId array = 0;
    RegisterId callback = 0;
    RegisterId length = 0;
    RegisterId guard = 0;
    std::size_t callback_arity = 0;
    enum class Comparison : std::uint8_t { Integer, Double };
    Comparison comparison = Comparison::Integer;
    frontend::TypeDescriptor element_type;
    const frontend::FunctionDescriptor* signature = nullptr;
    std::optional<RegisterId> validation_plan = std::nullopt;
  };
  LoweredValue LowerArrayTraversal(const ArrayAlgorithmRegisters& inputs,
                                   ArrayAlgorithm algorithm,
                                   const frontend::TypeDescriptor& receiver_type,
                                   const frontend::TypeDescriptor& result_type,
                                   frontend::SourceLocation location,
                                   std::optional<RegisterId> mapped_output = std::nullopt);
  LoweredValue LowerArraySortTraversal(const ArrayAlgorithmRegisters& inputs,
                                       const frontend::TypeDescriptor& result_type,
                                       frontend::SourceLocation location);
  RegisterId CheckArrayCallbackResult(RegisterId value,
                                      const ArrayAlgorithmRegisters& inputs,
                                      ArrayAlgorithm algorithm,
                                      const frontend::TypeDescriptor& result_type,
                                      frontend::SourceLocation location);
  RegisterId EmitPlanValidation(const ArrayAlgorithmRegisters& inputs,
                                RegisterId value,
                                std::optional<frontend::TypeDescriptor> type,
                                frontend::SourceLocation location);
  RegisterId EmitArrayLess(const ArrayAlgorithmRegisters& inputs,
                           std::pair<RegisterId, RegisterId> indices,
                           frontend::SourceLocation location);
  RegisterId EmitArrayValueLess(const ArrayAlgorithmRegisters& inputs,
                                std::pair<RegisterId, RegisterId> operands,
                                frontend::SourceLocation location);
  void LowerArrayInsertionSort(const ArrayAlgorithmRegisters& inputs,
                               frontend::SourceLocation location);
  RegisterId EmitArrayRunEnd(const ArrayAlgorithmRegisters& inputs,
                             std::pair<RegisterId, RegisterId> run,
                             frontend::SourceLocation location);
  void LowerArraySort(const ArrayAlgorithmRegisters& inputs,
                      RegisterId scratch,
                      frontend::SourceLocation location);
  void LowerArrayMergePass(RegisterId output,
                           const ArrayAlgorithmRegisters& inputs,
                           RegisterId width,
                           frontend::SourceLocation location);
  RegisterId EmitTemporary(Instruction instruction);
  std::uint32_t RegisterArrayType(const frontend::TypeDescriptor& type);

  LoweredValue LowerEnumMember(const frontend::Expression& expression,
                               const frontend::TypeDescriptor& type);

  LoweredValue LowerUnary(const frontend::Expression& expression);
  LoweredValue LowerIncrement(const frontend::Expression& expression);
  struct MutationTarget final {
    enum class Kind : std::uint8_t { Local, Global, Setter, Field, Member, Index };
    Kind kind = Kind::Local;
    LoweredValue value;
    LoweredValue receiver;
    std::uint32_t slot = 0;
    RegisterId index = 0;
  };
  [[nodiscard]] MutationTarget LowerMutationTarget(const frontend::Expression& target);
  void StoreMutationTarget(const MutationTarget& destination,
                           RegisterId value,
                           frontend::SourceLocation location);
  void EmitSetterCall(std::uint32_t target,
                      const LoweredValue& receiver,
                      RegisterId value,
                      frontend::SourceLocation location);
  [[nodiscard]] static Opcode SelectEqualityOpcode(const frontend::TypeDescriptor& type);

  [[nodiscard]] bool HasAssignments(const frontend::Expression& expression);
  LoweredValue LowerBinary(const frontend::Expression& expression);
  LoweredValue LowerIntegerAdditionChain(const frontend::Expression& expression);
  LoweredValue LowerCompoundAssignment(const frontend::Expression& expression);
  LoweredValue LowerBinaryValue(const frontend::Expression& expression);
  LoweredValue LowerConditional(const frontend::Expression& expression);
  // 两条分支分别延迟生成，保持短路求值；回调不做类型擦除或堆分配。
  template <class First, class Second>
  LoweredValue LowerBranchValue(const frontend::Expression& expression,
                                Opcode jump_opcode,
                                RegisterId condition,
                                First first,
                                Second second) {
    const auto location = expression.location;
    const auto result = AllocateRegister(location);
    const auto alternate_jump = current_function_->instructions.size();
    Emit({.opcode = jump_opcode, .lhs = condition, .location = location});
    const auto initial = first();
    Emit({.opcode = Opcode::Move, .dst = result, .lhs = initial.reg, .location = location});
    const auto end_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::Jump, .location = location});
    BindJump(alternate_jump);
    const auto alternative = second();
    Emit({.opcode = Opcode::Move, .dst = result, .lhs = alternative.reg, .location = location});
    BindJump(end_jump);
    return {.reg = result, .type = model_.expression_types.at(&expression)};
  }

  static Opcode SelectDoubleOpcode(frontend::TokenKind operation);
  static Opcode SelectBinaryOpcode(frontend::TokenKind operation,
                                   const frontend::TypeDescriptor& type);

  void LowerArguments(const frontend::Expression& expression, RegisterId base);
  void InitializeOptionalArguments(const frontend::FunctionDescriptor& function,
                                   RegisterId base,
                                   frontend::SourceLocation location);

  LoweredValue LowerInvocation(const frontend::Expression& expression,
                               const frontend::FunctionDescriptor& function,
                               std::uint32_t function_id,
                               const LoweredValue* receiver,
                               Opcode opcode = Opcode::Call);
  LoweredValue LowerLogical(const frontend::Expression& expression);

  const frontend::Program& source_;
  const frontend::SemanticModel& model_;
  const DeclarationBindings* declarations_;
  NativeProgramMode native_mode_;
  Program program_;
  frontend::Diagnostics diagnostics_;
  ClassMap class_ids_;
  absl::flat_hash_map<std::string_view, bool> instance_initialization_;
  FunctionMap function_ids_;
  absl::flat_hash_map<std::uint64_t, ClassId> callable_classes_;
  absl::flat_hash_map<std::string, std::uint32_t> interface_method_ids_;
  absl::flat_hash_map<std::string, std::uint32_t> global_ids_;
  absl::flat_hash_map<std::string, std::uint32_t> runtime_type_ids_;
  absl::flat_hash_map<std::uint32_t, RegisterId> binding_registers_;
  absl::flat_hash_map<std::string, std::uint32_t> collection_type_ids_;
  absl::flat_hash_map<std::string, std::uint32_t> array_type_ids_;
  absl::flat_hash_map<std::uint64_t, std::uint32_t> double_constant_ids_;
  absl::flat_hash_map<std::string, std::uint32_t> string_constant_ids_;
  absl::flat_hash_map<const frontend::Expression*, bool> assignment_effects_;
  struct CallValueType final {
    std::uint32_t function;
    RegisterId result;
    std::uint32_t target;
  };
  std::vector<CallValueType> call_value_types_;
  Function* current_function_ = nullptr;
  const frontend::Class* current_owner_ = nullptr;
  std::vector<LocalMap> scopes_;
  std::vector<ControlTargets> controls_;
  std::vector<Finally> finalizers_;
  std::vector<const frontend::Class*> pending_constructors_;
  absl::flat_hash_map<DeclarationId, std::uint32_t> field_initializers_;
  std::optional<std::uint32_t> current_handler_;
  RegisterId next_register_ = 0;
  std::vector<std::uint32_t> lambda_ids_;
};

}  // namespace vela::ir
