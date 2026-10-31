#pragma once

#include "base/array_algorithm.h"
#include "base/async_operation.h"
#include "base/builtin.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/member.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/declaration_bindings.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/persistent_ids.h"

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
          PersistentIds* persistent = nullptr,
          const DeclarationBindings* declarations = nullptr,
          NativeProgramMode native_mode = NativeProgramMode::Closed) noexcept
      : source_(source),
        model_(model),
        persistent_(persistent),
        declarations_(declarations),
        native_mode_(native_mode) {}

  // lowering 状态借用固定的 AST 和语义模型；执行上下文不允许复制或迁移。
  ~Lowerer() = default;
  Lowerer(const Lowerer&) = delete;
  Lowerer& operator=(const Lowerer&) = delete;
  Lowerer(Lowerer&&) = delete;
  Lowerer& operator=(Lowerer&&) = delete;

  [[nodiscard]] LoweringResult Lower();

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
    std::vector<frontend::Capture> captures;
    std::optional<LoweredValue> receiver;
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
  void BuildInterfaceSelectors();
  void BuildFunctionTable();
  std::uint32_t AddFunctionId(const std::string& name,
                              frontend::Function::Role role = frontend::Function::Role::Ordinary);
  enum class GlobalKind : std::uint8_t { Module, StaticField };
  void AddGlobalId(const std::string& name, GlobalKind kind);
  void StoreFunction(Function function);
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
  void LowerFunctionParameters(const frontend::Function& source,
                               const frontend::FunctionDescriptor& function_info,
                               const frontend::Class* owner,
                               const frontend::LambdaDescriptor* lambda);
  [[nodiscard]] bool HasInstanceInitialization(const frontend::ClassDescriptor& owner) const;
  [[nodiscard]] std::uint32_t ConstructorMaskCount(const frontend::ClassDescriptor& owner) const;
  void LowerConstructorInitializers(const frontend::ClassDescriptor& owner);
  void RegisterFunctionSignature(Function& function,
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
                                     std::span<const std::uint64_t> provided,
                                     frontend::SourceLocation location);
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
  void PublishMemberFunction(const frontend::TypeDescriptor& receiver,
                             std::string_view name,
                             MemberKind kind,
                             const frontend::FunctionDescriptor& signature,
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
  ClassId CallableClass(std::uint32_t function_id,
                        CallableMode mode,
                        const std::vector<frontend::Capture>& captures,
                        const frontend::TypeDescriptor& type);

  [[nodiscard]] const frontend::ClassDescriptor* FindClass(std::string_view name) const;
  [[nodiscard]] const Local* FindLocal(std::string_view name) const;
  [[nodiscard]] RegisterId AllocateRegister(frontend::SourceLocation location);
  [[nodiscard]] RegisterId AllocateRegisterWindow(std::size_t count,
                                                  frontend::SourceLocation location);
  [[nodiscard]] RegisterId AllocateTemporary(frontend::SourceLocation location);
  [[nodiscard]] static bool IsInteger(const frontend::TypeDescriptor& type);
  [[nodiscard]] static bool IsBoolean(const frontend::TypeDescriptor& type);
  [[nodiscard]] static std::optional<FieldId> FindField(const frontend::ClassDescriptor& class_info,
                                                        std::string_view name);
  void Emit(Instruction instruction);
  void RecordValueType(RegisterId reg, const frontend::TypeDescriptor& type);
  void RecordInstructionValueType(const Instruction& instruction);
  void RecordProgramValueTypes();
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
  void FinishNullSafe(std::optional<std::size_t> null_jump,
                      RegisterId result,
                      frontend::SourceLocation location);
  void LowerWhile(const frontend::Statement& statement);

  void LowerFor(const frontend::Statement& statement);
  void LowerForEach(const frontend::Statement& statement);

  LoweredValue LowerIntegerLiteral(const frontend::Expression& expression);
  LoweredValue LowerDoubleLiteral(const frontend::Expression& expression);

  LoweredValue LowerStringLiteral(const frontend::Expression& expression);
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
  void LowerBuiltinDefaults(Builtin builtin,
                            const frontend::Expression& expression,
                            RegisterId base);
  LoweredValue LowerAsyncCall(const frontend::Expression& expression);
  void LowerAsyncArguments(const frontend::Expression& expression,
                           AsyncOperation operation,
                           RegisterId base,
                           const std::optional<LoweredValue>& receiver);
  LoweredValue LowerSnapshotArray(const frontend::Expression& expression);
  LoweredValue LowerCollectionConstruction(const frontend::Expression& expression);
  std::uint32_t RegisterCollectionType(frontend::TypeDescriptor type);

  LoweredValue LowerArrayLiteral(const frontend::Expression& expression);
  LoweredValue LowerArrayMutation(const frontend::Expression& expression);
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
    RegisterId positions = 0;
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
  RegisterId EmitArrayLess(const ArrayAlgorithmRegisters& inputs,
                           std::pair<RegisterId, RegisterId> indices,
                           frontend::SourceLocation location);
  void LowerArraySort(const ArrayAlgorithmRegisters& inputs, frontend::SourceLocation location);
  void LowerArraySiftDown(RegisterId root,
                          const ArrayAlgorithmRegisters& inputs,
                          frontend::SourceLocation location);
  RegisterId EmitTemporary(Instruction instruction);
  std::uint32_t RegisterArrayType(const frontend::TypeDescriptor& type);

  LoweredValue LowerEnumMember(const frontend::Expression& expression,
                               const frontend::TypeDescriptor& type);

  LoweredValue LowerUnary(const frontend::Expression& expression);
  LoweredValue LowerIncrement(const frontend::Expression& expression);
  struct MutationTarget final {
    LoweredValue value;
    std::optional<std::uint32_t> global;
    std::optional<std::uint32_t> setter;
    std::optional<RegisterId> object;
    std::optional<RegisterId> index;
    std::optional<FieldId> field;
    std::optional<RegisterId> member;
  };
  [[nodiscard]] MutationTarget LowerMutationTarget(const frontend::Expression& target);
  void StoreMutationTarget(const frontend::Expression& target,
                           const MutationTarget& destination,
                           RegisterId value,
                           frontend::SourceLocation location);

  LoweredValue LowerBinary(const frontend::Expression& expression);
  LoweredValue LowerIntegerAdditionChain(const frontend::Expression& expression);
  LoweredValue LowerCompoundAssignment(const frontend::Expression& expression,
                                       std::string_view operation);
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
    const auto result = AllocateTemporary(location);
    const auto alternate_jump = current_function_->instructions.size();
    Emit({.opcode = jump_opcode, .lhs = condition, .location = location});
    const auto initial = first();
    Emit({.opcode = Opcode::Move, .dst = result, .lhs = initial.reg, .location = location});
    const auto end_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::Jump, .location = location});
    current_function_->instructions.at(alternate_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
    const auto alternative = second();
    Emit({.opcode = Opcode::Move, .dst = result, .lhs = alternative.reg, .location = location});
    current_function_->instructions.at(end_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
    return {.reg = result, .type = model_.expression_types.at(&expression)};
  }

  static std::optional<Opcode> SelectDoubleOpcode(std::string_view operation);
  static std::optional<Opcode> SelectBinaryOpcode(std::string_view operation,
                                                  const frontend::TypeDescriptor& type);

  void LowerArguments(const frontend::Expression& expression, RegisterId base);

  LoweredValue LowerInvocation(const frontend::Expression& expression,
                               const frontend::FunctionDescriptor& function,
                               std::uint32_t function_id,
                               const LoweredValue* receiver,
                               Opcode opcode = Opcode::Call);
  LoweredValue LowerLogical(const frontend::Expression& expression);

  const frontend::Program& source_;
  const frontend::SemanticModel& model_;
  PersistentIds* persistent_;
  const DeclarationBindings* declarations_;
  NativeProgramMode native_mode_;
  Program program_;
  frontend::Diagnostics diagnostics_;
  ClassMap class_ids_;
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
  Function* current_function_ = nullptr;
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
