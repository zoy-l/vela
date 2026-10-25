#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/persistent_ids.h"

namespace vela::ir {

struct LoweringResult final {
  Program program;
  frontend::Diagnostics diagnostics;

  [[nodiscard]] auto Succeeded() const noexcept -> bool { return diagnostics.empty(); }
};

class Lowerer final {
 public:
  Lowerer(const frontend::Program& source,
          const frontend::SemanticModel& model,
          PersistentIds* persistent = nullptr) noexcept
      : source_(source), model_(model), persistent_(persistent) {}

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
    bool cell = false;
  };

  struct LoweredValue final {
    RegisterId reg = 0;
    frontend::TypeDescriptor type;
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
  void BuildInterfaceSelectors();
  void BuildFunctionTable();
  auto AddFunctionId(const std::string& name,
                     frontend::Function::Role role = frontend::Function::Role::Ordinary)
      -> std::uint32_t;
  enum class GlobalKind : std::uint8_t { Module, StaticField };
  void AddGlobalId(const std::string& name, GlobalKind kind);
  void StoreFunction(Function function);
  void LowerInitializer(std::string name,
                        const frontend::Expression& initializer,
                        const frontend::TypeDescriptor& type,
                        frontend::SourceLocation location);
  void LowerFunction(const frontend::Function& source,
                     const frontend::Class* owner = nullptr,
                     const frontend::LambdaDescriptor* lambda = nullptr);
  void LowerStatement(const frontend::Statement& statement);
  void LowerBlock(const frontend::Statement& statement);
  void LowerScopedStatement(const frontend::Statement& statement);
  void LowerSwitch(const frontend::Statement& statement);
  void LowerTry(const frontend::Statement& statement);
  void LowerReturn(const frontend::Statement& statement);
  void LowerControlExit(const frontend::Statement& statement);
  auto LowerFinalizers(std::size_t depth) -> Completion;
  auto LowerConstantLabel(const frontend::Expression& source) -> RegisterId;
  auto LowerMatch(const frontend::Expression& expression) -> LoweredValue;

  [[nodiscard]] auto LowerExpression(const frontend::Expression& expression) -> LoweredValue;
  [[nodiscard]] auto SnapshotBefore(LoweredValue value, const frontend::Expression& later)
      -> LoweredValue;
  [[nodiscard]] auto LowerAssignment(const frontend::Expression& expression) -> LoweredValue;
  [[nodiscard]] auto LowerConstruction(const frontend::Expression& expression,
                                       const frontend::TypeDescriptor& expected) -> LoweredValue;
  [[nodiscard]] auto LowerMember(const frontend::Expression& expression) -> LoweredValue;
  [[nodiscard]] auto LowerIndex(const frontend::Expression& expression) -> LoweredValue;
  [[nodiscard]] auto LowerCall(const frontend::Expression& expression) -> LoweredValue;
  auto LowerFunctionValue(const frontend::Expression& expression) -> LoweredValue;
  auto LowerIndirectCall(const frontend::Expression& expression) -> LoweredValue;
  auto LowerInterfaceCall(const frontend::Expression& expression) -> LoweredValue;
  auto LowerTypeOperation(const frontend::Expression& expression) -> LoweredValue;
  auto RegisterRuntimeType(const frontend::TypeDescriptor& type) -> std::uint32_t;
  auto LowerExpected(const frontend::Expression& expression) -> LoweredValue;
  auto RegisterExpectedType(frontend::TypeDescriptor type) -> ClassId;
  auto ReadLocal(const Local& local, frontend::SourceLocation location) -> LoweredValue;
  auto CallableClass(std::uint32_t function_id,
                     CallableMode mode,
                     const std::vector<frontend::Capture>& captures) -> ClassId;

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
                    frontend::SourceLocation location,
                    const void* declaration = nullptr) -> bool;

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
  auto LowerStringInterpolation(const frontend::Expression& expression) -> LoweredValue;
  auto LowerBuiltin(const frontend::Expression& expression) -> LoweredValue;
  auto LowerSnapshotArray(const frontend::Expression& expression) -> LoweredValue;
  auto LowerCollectionConstruction(const frontend::Expression& expression) -> LoweredValue;
  auto RegisterCollectionType(frontend::TypeDescriptor type) -> std::uint32_t;

  auto LowerArrayLiteral(const frontend::Expression& expression) -> LoweredValue;
  auto LowerArrayAlgorithm(const frontend::Expression& expression) -> LoweredValue;
  auto EmitCallback(RegisterId callback,
                    std::span<const RegisterId> arguments,
                    frontend::SourceLocation location) -> RegisterId;
  struct ArrayAlgorithmRegisters final {
    RegisterId array;
    RegisterId callback;
    RegisterId length;
  };
  void LowerArraySort(const ArrayAlgorithmRegisters& inputs, frontend::SourceLocation location);
  void LowerArraySiftDown(RegisterId root,
                          const ArrayAlgorithmRegisters& inputs,
                          frontend::SourceLocation location);
  auto EmitTemporary(Instruction instruction) -> RegisterId;
  auto RegisterArrayType(const frontend::TypeDescriptor& type) -> std::uint32_t;

  auto LowerEnumMember(const frontend::Expression& expression, const frontend::TypeDescriptor& type)
      -> LoweredValue;

  auto LowerUnary(const frontend::Expression& expression) -> LoweredValue;

  auto LowerBinary(const frontend::Expression& expression) -> LoweredValue;

  auto LowerInvocation(const frontend::Expression& expression,
                       const frontend::FunctionDescriptor& function,
                       std::uint32_t function_id,
                       const LoweredValue* receiver,
                       Opcode opcode = Opcode::Call) -> LoweredValue;
  auto LowerLogical(const frontend::Expression& expression) -> LoweredValue;

  const frontend::Program& source_;
  const frontend::SemanticModel& model_;
  PersistentIds* persistent_;
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
  std::optional<std::uint32_t> current_handler_;
  RegisterId next_register_ = 0;
  std::vector<std::uint32_t> lambda_ids_;
};

}  // namespace vela::ir
