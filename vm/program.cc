#include "vm/program.h"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/async_operation.h"
#include "base/builtin.h"
#include "base/callable.h"
#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "base/member.h"
#include "base/utf8.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "vm/bytecode.h"

namespace vela::vm {
namespace {

constexpr std::size_t kLayoutPoolCount = 5;

struct LayoutKey final {
  std::array<const void*, kLayoutPoolCount> addresses;
  std::array<std::size_t, kLayoutPoolCount> sizes;

  bool operator==(const LayoutKey&) const = default;

  template <typename H>
  friend H AbslHashValue(H hash, const LayoutKey& key) {
    return H::combine(std::move(hash), key.addresses, key.sizes);
  }
};

bool IsRegister(std::uint8_t index, const Function& function) noexcept {
  return index < function.register_count;
}

bool IsRegisterPair(std::uint8_t first, std::uint8_t second, const Function& function) noexcept {
  return IsRegister(first, function) && IsRegister(second, function);
}

bool IsValidAsyncInstruction(Instruction instruction, const Function& function) noexcept {
  const auto operand_a = DecodeA(instruction);
  const auto operand_b = DecodeB(instruction);
  const auto operand_c = DecodeC(instruction);
  switch (DecodeOp(instruction)) {
    case OpCode::AsyncCall:
      return IsRegister(operand_a, function) &&
             operand_b < static_cast<std::uint8_t>(AsyncOperation::Count) &&
             static_cast<std::size_t>(operand_c) +
                     AsyncArity(static_cast<AsyncOperation>(operand_b)) <=
                 function.register_count;
    case OpCode::NewFuture:
      return IsRegisterPair(operand_a, operand_b, function) && operand_c <= 1;
    case OpCode::SleepFuture:
      return IsRegisterPair(operand_a, operand_b, function);
    case OpCode::Await:
      return function.is_async && IsRegisterPair(operand_a, operand_b, function);
    default:
      return false;
  }
}

bool IsValidInstruction(Instruction instruction,
                        const Function& function,
                        std::span<const FunctionStorage> functions,
                        std::size_t global_count) noexcept {
  const OpCode opcode = DecodeOp(instruction);
  const auto operand_a = DecodeA(instruction);
  const auto operand_b = DecodeB(instruction);
  const auto operand_c = DecodeC(instruction);
  switch (opcode) {
    case OpCode::LoadNull:
    case OpCode::LoadBool:
    case OpCode::LoadInt:
    case OpCode::Catch:
    case OpCode::Return:
      return IsRegister(operand_a, function);
    case OpCode::NewArray:
      return IsRegister(operand_a, function) && operand_b < function.arrays.size() &&
             function.arrays.subspan(operand_b).front() != nullptr;
    case OpCode::NewDynamicArray:
      return IsRegister(operand_a, function) && IsRegister(operand_c, function) &&
             operand_b < function.arrays.size() &&
             function.arrays.subspan(operand_b).front() != nullptr;
    case OpCode::ExceptionValue:
      return IsRegister(operand_a, function) && IsRegister(operand_b, function);
    case OpCode::Throw:
    case OpCode::Rethrow:
      return IsRegister(operand_b, function);
    case OpCode::NewCollection:
      return IsRegister(operand_a, function) && operand_b < function.collections.size() &&
             function.collections.subspan(operand_b).front() != nullptr;
    case OpCode::LoadDouble:
      return IsRegister(operand_a, function) &&
             DecodeUnsigned16(instruction) < function.doubles.size();
    case OpCode::LoadIntConstant:
      return IsRegister(operand_a, function) &&
             DecodeUnsigned16(instruction) < function.integers.size();
    case OpCode::GetGlobal:
    case OpCode::SetGlobal:
      return IsRegister(operand_a, function) && operand_b < global_count;
    case OpCode::Move:
    case OpCode::NegInt:
    case OpCode::BitNotInt:
    case OpCode::NegDouble:
    case OpCode::NotBool:
    case OpCode::GetField:
      return IsRegister(operand_a, function) && IsRegister(operand_b, function);
    case OpCode::IsType:
    case OpCode::CheckedCast:
      return IsRegister(operand_a, function) && IsRegister(operand_b, function) &&
             operand_c < function.types.size();
    case OpCode::NewOk:
    case OpCode::NewErr:
      return IsRegister(operand_a, function) && IsRegister(operand_c, function) &&
             operand_b < function.classes.size() &&
             function.classes.subspan(operand_b).front() != nullptr &&
             function.classes.subspan(operand_b).front()->field_count() == 1;
    case OpCode::ExpectedHasValue:
      return IsRegister(operand_a, function) && IsRegister(operand_b, function);
    case OpCode::ExpectedValue:
      return IsRegister(operand_a, function) && IsRegister(operand_b, function) && operand_c <= 1;
    case OpCode::AddInt:
    case OpCode::AddDouble:
    case OpCode::SubDouble:
    case OpCode::MulDouble:
    case OpCode::DivDouble:
    case OpCode::EqualDouble:
    case OpCode::NotEqualDouble:
    case OpCode::LessDouble:
    case OpCode::LessEqualDouble:
    case OpCode::GreaterDouble:
    case OpCode::GreaterEqualDouble:
    case OpCode::SubInt:
    case OpCode::MulInt:
    case OpCode::DivInt:
    case OpCode::ModInt:
    case OpCode::BitAndInt:
    case OpCode::BitOrInt:
    case OpCode::BitXorInt:
    case OpCode::ShiftLeftInt:
    case OpCode::ShiftRightInt:
    case OpCode::ShiftUnsignedRightInt:
    case OpCode::EqualInt:
    case OpCode::NotEqualInt:
    case OpCode::EqualBool:
    case OpCode::NotEqualBool:
    case OpCode::EqualValue:
    case OpCode::NotEqualValue:
    case OpCode::LessInt:
    case OpCode::LessEqualInt:
    case OpCode::GreaterInt:
    case OpCode::GreaterEqualInt:
    case OpCode::AndBool:
    case OpCode::OrBool:
    case OpCode::StringConcat:
    case OpCode::EqualString:
    case OpCode::NotEqualString:
    case OpCode::GetIndex:
    case OpCode::SetIndex:
      return IsRegister(operand_a, function) && IsRegister(operand_b, function) &&
             IsRegister(operand_c, function);
    case OpCode::Call:
      return IsRegister(operand_a, function) && operand_b < functions.size() &&
             static_cast<std::size_t>(operand_c) +
                     functions.subspan(operand_b).front().parameter_count <=
                 function.register_count;
    case OpCode::CallIndirect:
      return IsRegister(operand_a, function) && IsRegister(operand_b, function) &&
             operand_c <= function.register_count;
    case OpCode::CallInterface:
      return IsRegister(operand_a, function) && IsRegister(operand_c, function);
    case OpCode::BuiltinCall:
      return IsRegister(operand_a, function) &&
             operand_b < static_cast<std::uint8_t>(Builtin::Count) &&
             static_cast<std::size_t>(operand_c) + BuiltinArity(static_cast<Builtin>(operand_b)) <=
                 function.register_count;
    case OpCode::AsyncCall:
      return IsValidAsyncInstruction(instruction, function);
    case OpCode::StringInterpolate:
      return IsRegister(operand_a, function) &&
             static_cast<std::size_t>(operand_b) + operand_c <= function.register_count;
    case OpCode::Jump:
      return true;
    case OpCode::JumpIfFalse:
    case OpCode::JumpIfNull:
      return IsRegister(operand_a, function);
    case OpCode::NewInstance:
      return IsRegister(operand_a, function) && operand_b < function.classes.size() &&
             function.classes.subspan(operand_b).front() != nullptr;
    case OpCode::NewFuture:
    case OpCode::SleepFuture:
    case OpCode::Await:
      return IsValidAsyncInstruction(instruction, function);
    case OpCode::LoadString:
      return IsRegister(operand_a, function) &&
             DecodeUnsigned16(instruction) < function.strings.size() &&
             function.strings.subspan(DecodeUnsigned16(instruction)).front() != nullptr;
    case OpCode::Print:
      return IsRegister(operand_b, function);
    case OpCode::SetField:
      return IsRegister(operand_a, function) && IsRegister(operand_c, function);
  }
  return false;
}

bool HasMatchingCallableSignature(const runtime::ClassLayout& layout,
                                  const FunctionStorage& function,
                                  CallableMode mode) {
  const auto* expected = layout.callable_type();
  if (expected == nullptr) {
    return true;
  }
  if (!function.signature) {
    return false;
  }
  const auto& signature = *function.signature;
  const auto hidden = mode == CallableMode::Plain ? 0U : 1U;
  if (signature.hidden_parameters > hidden ||
      expected->result.has_value() != signature.result.has_value()) {
    return false;
  }
  const auto skipped = hidden - signature.hidden_parameters;
  if (signature.parameters.size() < skipped ||
      expected->parameters.size() != signature.parameters.size() - skipped) {
    return false;
  }
  const auto types = function.View().types;
  if (signature.result && *expected->result != types.subspan(*signature.result).front()) {
    return false;
  }
  for (std::size_t index = 0; index < expected->parameters.size(); ++index) {
    const auto& actual = signature.parameters.at(index + skipped);
    const auto& parameter = expected->parameters.at(index);
    if (parameter.type != types.subspan(actual.type).front() ||
        parameter.optional != actual.optional || parameter.rest != actual.rest) {
      return false;
    }
  }
  return true;
}

std::expected<void, ValidationError> ValidateClassLayout(const runtime::ClassLayout* layout,
                                                         std::span<const FunctionStorage> storage,
                                                         std::size_t index) {
  for (const auto* interface : layout->interfaces()) {
    if (interface == nullptr) {
      return std::unexpected(ValidationError{
          .function = index,
          .instruction = 0,
          .message = "invalid implemented interface identity",
      });
    }
  }
  for (const auto target : layout->dispatch_targets()) {
    if (target != kInvalidFunctionTarget &&
        (target >= storage.size() || storage.subspan(target).front().parameter_count == 0)) {
      return std::unexpected(ValidationError{
          .function = index,
          .instruction = 0,
          .message = "invalid interface dispatch target",
      });
    }
  }
  if (!layout->callable()) {
    return {};
  }
  const auto callable = *layout->callable();
  if (callable.mode != CallableMode::Plain && callable.mode != CallableMode::Closure &&
      callable.mode != CallableMode::BoundMethod &&
      callable.mode != CallableMode::InterfaceMethod) {
    return std::unexpected(
        ValidationError{.function = index, .instruction = 0, .message = "invalid callable mode"});
  }
  if (callable.mode == CallableMode::InterfaceMethod) {
    if (layout->field_count() == 0) {
      return std::unexpected(ValidationError{
          .function = index,
          .instruction = 0,
          .message = "interface method requires a bound receiver",
      });
    }
  } else if (callable.function >= storage.size() ||
             (callable.mode != CallableMode::Plain &&
              storage.subspan(callable.function).front().parameter_count == 0) ||
             (callable.mode == CallableMode::BoundMethod && layout->field_count() == 0) ||
             !HasMatchingCallableSignature(*layout, storage.subspan(callable.function).front(),
                                           callable.mode)) {
    return std::unexpected(
        ValidationError{.function = index, .instruction = 0, .message = "invalid callable layout"});
  }
  return {};
}

std::expected<void, ValidationError> ValidateFunctionCode(std::size_t index,
                                                          const Function& function,
                                                          std::span<const FunctionStorage> storage,
                                                          std::size_t global_count) {
  std::uint32_t previous_end = 0;
  for (const auto& handler : function.handlers) {
    if (handler.begin < previous_end || handler.begin >= handler.end ||
        handler.end > function.code.size() || handler.target >= function.code.size() ||
        DecodeOp(function.code.subspan(handler.target).front()) != OpCode::Catch) {
      return std::unexpected(ValidationError{
          .function = index,
          .instruction = handler.begin,
          .message = "invalid exception handler",
      });
    }
    previous_end = handler.end;
  }
  std::optional<std::uint32_t> previous;
  for (const auto& mapping : function.debug.mappings) {
    const auto location_count = function.native != nullptr ? 1U : function.code.size();
    if (mapping.instruction >= location_count ||
        mapping.location.source_id >= function.debug.source_paths.size() ||
        mapping.location.line == 0 || mapping.location.column == 0 ||
        (previous ? mapping.instruction <= *previous : mapping.instruction != 0)) {
      return std::unexpected(ValidationError{
          .function = index,
          .instruction = mapping.instruction,
          .message = "invalid source mapping",
      });
    }
    previous = mapping.instruction;
  }
  for (std::size_t position = 0; position < function.code.size(); ++position) {
    const auto instruction = function.code.subspan(position).front();
    if (!IsValidInstruction(instruction, function, storage, global_count)) {
      return std::unexpected(ValidationError{
          .function = index,
          .instruction = position,
          .message = "invalid instruction operand",
      });
    }
    const auto opcode = DecodeOp(instruction);
    if (opcode == OpCode::Jump || opcode == OpCode::JumpIfFalse) {
      const auto target = static_cast<std::int64_t>(position) + 1 + DecodeImm16(instruction);
      if (target < 0 || std::cmp_greater_equal(target, function.code.size())) {
        return std::unexpected(ValidationError{
            .function = index,
            .instruction = position,
            .message = "jump target is outside the function",
        });
      }
    }
  }
  if (function.native != nullptr) {
    return {};
  }
  const auto last = DecodeOp(function.code.back());
  if (last != OpCode::Return && last != OpCode::Jump && last != OpCode::Throw &&
      last != OpCode::Rethrow) {
    return std::unexpected(ValidationError{
        .function = index,
        .instruction = function.code.size() - 1,
        .message = "function can fall off its bytecode",
    });
  }
  return {};
}

enum class AccessorKind : std::uint8_t { Getter, Setter };

bool IsValidAccessor(std::span<const FunctionStorage> functions,
                     std::uint32_t target,
                     AccessorKind kind) {
  if (target >= functions.size()) {
    return false;
  }
  const auto& function = functions.subspan(target).front();
  const auto& signature = function.signature;
  const auto count = kind == AccessorKind::Getter ? 0U : 1U;
  return signature && signature->hidden_parameters == 1 && signature->parameters.size() == count &&
         signature->result.has_value() == (count == 0);
}

bool IsValidMemberTarget(const MemberDefinition& member,
                         const LayoutTables& table,
                         std::span<const FunctionStorage> functions) {
  const auto types = table.types();
  const auto classes = table.classes();
  const auto& receiver = types.subspan(member.receiver_type).front();
  const auto* instance = receiver.instance;
  switch (member.kind) {
    case MemberKind::Field:
      if (receiver.kind != runtime::ValueKind::Object ||
          receiver.object_kind != runtime::ObjectKind::Instance || instance == nullptr ||
          instance->IsEnum() || !member.type || *member.type >= types.size() ||
          member.target >= instance->field_count() || member.setter) {
        return false;
      }
      break;
    case MemberKind::Method: {
      if (member.type || member.setter || member.target >= classes.size()) {
        return false;
      }
      const auto* wrapper = classes.subspan(member.target).front();
      if (wrapper == nullptr) {
        return false;
      }
      const auto& callable = wrapper->callable();
      if (!callable || callable->mode != CallableMode::BoundMethod ||
          callable->function >= functions.size()) {
        return false;
      }
      const auto& function = functions.subspan(callable->function).front();
      const auto& signature = function.signature;
      if (!signature || signature->hidden_parameters != 1) {
        return false;
      }
      break;
    }
    case MemberKind::Property:
      if (member.type || (member.target == kInvalidFunctionTarget && !member.setter) ||
          (member.target != kInvalidFunctionTarget &&
           !IsValidAccessor(functions, member.target, AccessorKind::Getter)) ||
          (member.setter && !IsValidAccessor(functions, *member.setter, AccessorKind::Setter))) {
        return false;
      }
      break;
    default:
      return false;
  }
  return true;
}

std::expected<void, ValidationError> ValidateMembers(const LayoutTables& table,
                                                     std::span<const FunctionStorage> functions,
                                                     std::size_t owner) {
  absl::flat_hash_set<std::pair<std::uint32_t, std::string_view>> names;
  for (const auto& member : table.members()) {
    if (!table.IsCanonicalReceiverType(member.receiver_type) || member.name.empty() ||
        InvalidUtf8Offset(member.name) ||
        !names.emplace(member.receiver_type, std::string_view(member.name)).second ||
        !IsValidMemberTarget(member, table, functions)) {
      return std::unexpected(ValidationError{
          .function = owner,
          .instruction = 0,
          .message = "invalid dynamic member metadata",
      });
    }
  }
  return {};
}

std::expected<void, ValidationError> ValidateCallSignature(const FunctionStorage& stored,
                                                           std::size_t index) {
  if (!stored.signature) {
    return {};
  }
  const auto function = stored.View();
  const auto& signature = *stored.signature;
  const auto type_count = function.types.size();
  if (signature.hidden_parameters > 1 ||
      signature.parameters.size() + signature.hidden_parameters != function.parameter_count ||
      (signature.result && *signature.result >= type_count)) {
    return std::unexpected(ValidationError{
        .function = index,
        .instruction = 0,
        .message = "invalid call signature",
    });
  }
  absl::flat_hash_set<std::string_view> names;
  for (std::size_t parameter_index = 0; parameter_index < signature.parameters.size();
       ++parameter_index) {
    const auto& parameter = signature.parameters.at(parameter_index);
    if (parameter.type >= type_count || parameter.name.empty() ||
        InvalidUtf8Offset(parameter.name) || !names.insert(parameter.name).second ||
        (parameter.rest &&
         (parameter.optional || parameter_index + 1 != signature.parameters.size() ||
          function.types.subspan(parameter.type).front().array == nullptr))) {
      return std::unexpected(ValidationError{
          .function = index,
          .instruction = 0,
          .message = "invalid call parameter",
      });
    }
  }
  return {};
}

bool IsNativeType(const runtime::TypeConstraint& type, runtime::NativeType native) {
  if (native == runtime::NativeType::NullableString) {
    return type.kind == runtime::ValueKind::String && type.nullable;
  }
  if (type.nullable) {
    return false;
  }
  switch (native) {
    case runtime::NativeType::Boolean:
      return type.kind == runtime::ValueKind::Boolean;
    case runtime::NativeType::Integer:
      return type.kind == runtime::ValueKind::Integer;
    case runtime::NativeType::Double:
      return type.kind == runtime::ValueKind::Double;
    case runtime::NativeType::String:
      return type.kind == runtime::ValueKind::String;
    case runtime::NativeType::Future:
      return type.kind == runtime::ValueKind::Object &&
             type.object_kind == runtime::ObjectKind::Future && type.signature != nullptr;
    case runtime::NativeType::CancellationToken:
      return type.kind == runtime::ValueKind::Object &&
             type.object_kind == runtime::ObjectKind::CancellationToken;
    case runtime::NativeType::Void:
    case runtime::NativeType::NullableString:
      return false;
  }
  return false;
}

std::expected<void, ValidationError> ValidateNativeSignature(const FunctionStorage& stored,
                                                             std::size_t index) {
  if (stored.native == nullptr || !stored.signature) {
    return {};
  }
  const auto& signature = *stored.signature;
  const auto types = stored.View().types;
  const auto& native = *stored.native;
  const auto result_matches =
      signature.result
          ? IsNativeType(types.subspan(*signature.result).front(), native.result_type())
          : native.result_type() == runtime::NativeType::Void;
  if (signature.hidden_parameters != 0 || !result_matches) {
    return std::unexpected(ValidationError{
        .function = index,
        .instruction = 0,
        .message = "native call signature mismatch",
    });
  }
  const auto parameters = native.parameters();
  for (std::size_t parameter = 0; parameter < parameters.size(); ++parameter) {
    const auto& type = types.subspan(signature.parameters.at(parameter).type).front();
    if (!IsNativeType(type, parameters.subspan(parameter).front())) {
      return std::unexpected(ValidationError{
          .function = index,
          .instruction = 0,
          .message = "native parameter signature mismatch",
      });
    }
  }
  return {};
}

std::expected<void, ValidationError> ValidateFunctionMetadata(
    const FunctionStorage& stored,
    std::size_t index,
    absl::flat_hash_set<const std::vector<std::string>*>& validated_paths) {
  const auto function = stored.View();
  const auto* paths = stored.source_paths.get();
  const auto text_is_invalid =
      InvalidUtf8Offset(function.debug.name) ||
      (stored.native != nullptr && InvalidUtf8Offset(stored.native->name()));
  const auto paths_are_invalid = paths != nullptr && validated_paths.insert(paths).second &&
                                 std::ranges::any_of(*paths, [](const auto& path) -> bool {
                                   return InvalidUtf8Offset(path).has_value();
                                 });
  if (text_is_invalid || paths_are_invalid) {
    return std::unexpected(ValidationError{
        .function = index,
        .instruction = 0,
        .message = "program metadata is not valid UTF-8",
    });
  }
  const auto invalid_body =
      function.native != nullptr
          ? !function.code.empty() || !function.handlers.empty() || function.is_async ||
                function.parameter_count != function.native->parameters().size()
          : function.code.empty();
  if (invalid_body || function.register_count == 0 ||
      function.register_count > kMaxFrameRegisters ||
      function.parameter_count > function.register_count) {
    return std::unexpected(ValidationError{
        .function = index,
        .instruction = 0,
        .message = "invalid function metadata",
    });
  }
  if (auto valid = ValidateCallSignature(stored, index); !valid) {
    return valid;
  }
  return ValidateNativeSignature(stored, index);
}

std::expected<void, ValidationError> ValidateInterfaceSignatures(
    const absl::flat_hash_set<const runtime::ClassLayout*>& classes,
    std::span<const FunctionStorage> functions) {
  absl::flat_hash_map<std::size_t, const runtime::ClassLayout*> selectors;
  for (const auto* layout : classes) {
    const auto callable = layout->callable();
    if (!callable || (*callable).mode != CallableMode::InterfaceMethod ||
        layout->callable_type() == nullptr) {
      continue;
    }
    const auto [entry, inserted] = selectors.emplace((*callable).function, layout);
    if (!inserted && entry->second->callable_type() != layout->callable_type()) {
      return std::unexpected(ValidationError{
          .function = 0,
          .instruction = 0,
          .message = "interface selector has inconsistent callable signatures",
      });
    }
  }
  for (const auto* layout : classes) {
    const auto targets = layout->dispatch_targets();
    for (std::size_t selector = 0; selector < targets.size(); ++selector) {
      const auto target = targets.subspan(selector).front();
      const auto callable = selectors.find(selector);
      if (target != kInvalidFunctionTarget && callable != selectors.end() &&
          !HasMatchingCallableSignature(*callable->second, functions.subspan(target).front(),
                                        CallableMode::InterfaceMethod)) {
        return std::unexpected(ValidationError{
            .function = target,
            .instruction = 0,
            .message = "interface implementation disagrees with its callable signature",
        });
      }
    }
  }
  return {};
}

absl::flat_hash_set<runtime::ClassLayout*> IndexCallables(
    std::span<const FunctionStorage> functions) {
  absl::flat_hash_set<runtime::ClassLayout*> callable_layouts;
  absl::flat_hash_set<const LayoutTables*> indexed;
  for (const auto& function : functions) {
    const auto* table = function.layouts.get();
    if (table == nullptr || !indexed.insert(table).second) {
      continue;
    }
    for (auto* layout : table->classes()) {
      if (layout != nullptr && layout->callable()) {
        callable_layouts.insert(layout);
      }
    }
  }
  return callable_layouts;
}

std::expected<const runtime::CodeIdentity*, ValidationError> BindCodeIdentity(
    std::span<const FunctionStorage> functions,
    runtime::Isolate& isolate,
    const runtime::CodeIdentity* identity) {
  const auto callables = IndexCallables(functions);
  for (const auto* layout : callables) {
    const auto* existing = layout->code_identity();
    if (existing == nullptr) {
      continue;
    }
    if (identity != nullptr && identity != existing) {
      return std::unexpected(ValidationError{
          .function = 0,
          .instruction = 0,
          .message = "callable layouts belong to different code identities",
      });
    }
    identity = existing;
  }
  if (identity == nullptr) {
    identity = isolate.CreateCodeIdentity();
  }
  for (auto* layout : callables) {
    layout->BindCodeIdentity(*identity);
  }
  return identity;
}

std::expected<void, ValidationError> ValidateFunctions(
    std::span<const FunctionStorage> storage,
    std::span<const std::uint32_t> initializers) {
  absl::flat_hash_set<const runtime::ClassLayout*> validated_layouts;
  absl::flat_hash_set<const LayoutTables*> validated_tables;
  absl::flat_hash_set<const std::vector<std::string>*> validated_paths;
  for (std::size_t index = 0; index < storage.size(); ++index) {
    const auto& stored = storage.subspan(index).front();
    if (auto valid = ValidateFunctionMetadata(stored, index, validated_paths); !valid) {
      return valid;
    }
  }
  for (std::size_t index = 0; index < storage.size(); ++index) {
    const auto& stored = storage.subspan(index).front();
    const auto function = stored.View();
    const auto* table = stored.layouts.get();
    const auto first_table = validated_tables.insert(table).second;
    if (first_table && table != nullptr) {
      if (auto valid = ValidateMembers(*table, storage, index); !valid) {
        return valid;
      }
    }
    const auto classes = first_table ? function.classes : std::span<runtime::ClassLayout* const>();
    for (const auto* layout : classes) {
      if (layout == nullptr || !validated_layouts.insert(layout).second) {
        continue;
      }
      if (auto valid = ValidateClassLayout(layout, storage, index); !valid) {
        return valid;
      }
    }
    if (auto valid = ValidateFunctionCode(index, function, storage, initializers.size()); !valid) {
      return valid;
    }
  }
  return ValidateInterfaceSignatures(validated_layouts, storage);
}

}  // namespace

LayoutTables::LayoutTables(std::vector<runtime::ClassLayout*> classes,
                           std::vector<const runtime::ArrayLayout*> arrays,
                           std::vector<const runtime::CollectionLayout*> collections,
                           std::vector<runtime::TypeConstraint> types,
                           std::vector<MemberDefinition> members)
    : classes_(std::move(classes)),
      arrays_(std::move(arrays)),
      collections_(std::move(collections)),
      types_(std::move(types)),
      members_(std::move(members)) {
  for (std::size_t index = 0; index < types_.size(); ++index) {
    const auto& type = types_.at(index);
    if (!type.nullable && !type.dynamic && type.interface == nullptr && !type.callable) {
      receiver_indices_.try_emplace(Key(type), static_cast<std::uint32_t>(index));
    }
  }
  for (std::size_t index = 0; index < members_.size(); ++index) {
    const auto& member = members_.at(index);
    member_indices_.try_emplace(std::pair(member.receiver_type, std::string_view(member.name)),
                                index);
  }
}

LayoutTables::ReceiverKey LayoutTables::Key(const runtime::TypeConstraint& type) {
  return {type.kind, type.object_kind, type.instance, type.array, type.collection, type.signature};
}

bool LayoutTables::IsCanonicalReceiverType(std::uint32_t index) const {
  if (index >= types_.size()) {
    return false;
  }
  const auto found = receiver_indices_.find(Key(types_.at(index)));
  return found != receiver_indices_.end() && found->second == index;
}

const MemberDefinition* LayoutTables::FindMember(runtime::Value receiver,
                                                 std::string_view name) const {
  const auto key = Key(runtime::Heap::TypeOf(receiver));
  const auto type = receiver_indices_.find(key);
  if (type == receiver_indices_.end()) {
    return nullptr;
  }
  const auto member = member_indices_.find(std::pair(type->second, name));
  return member == member_indices_.end() ? nullptr : &members_.at(member->second);
}

Function FunctionStorage::View() const noexcept {
  return {
      .code = code,
      .classes = layouts != nullptr ? layouts->classes() : std::span<runtime::ClassLayout* const>(),
      .strings = strings != nullptr ? std::span<runtime::StringObject* const>(strings->values)
                                    : std::span<runtime::StringObject* const>(),
      .arrays =
          layouts != nullptr ? layouts->arrays() : std::span<const runtime::ArrayLayout* const>(),
      .doubles = doubles,
      .register_count = register_count,
      .parameter_count = parameter_count,
      .is_async = is_async,
      .types = layouts != nullptr ? layouts->types() : std::span<const runtime::TypeConstraint>(),
      .collections = layouts != nullptr ? layouts->collections()
                                        : std::span<const runtime::CollectionLayout* const>(),
      .debug =
          {
              .name = name,
              .source_paths = source_paths != nullptr ? std::span(*source_paths)
                                                      : std::span<const std::string>(),
              .mappings = source_mappings,
          },
      .native = native.get(),
      .handlers = handlers,
      .integers = integers,
      .signature = signature ? &*signature : nullptr,
      .members = layouts != nullptr ? &layouts->members() : nullptr,
  };
}

std::expected<Program, ValidationError> Program::Create(
    std::vector<FunctionStorage> functions,
    runtime::Isolate& isolate,
    std::vector<std::uint32_t> global_initializers,
    std::shared_ptr<GlobalStorage> globals,
    const runtime::CodeIdentity* code_identity) {
  for (auto& function : functions) {
    if (function.signature) {
      function.signature->IndexParameters();
    }
  }
  const auto valid = ValidateFunctions(functions, global_initializers);
  if (!valid) {
    return std::unexpected(valid.error());
  }
  for (const auto initializer : global_initializers) {
    if (initializer >= functions.size() ||
        std::span(functions).subspan(initializer).front().parameter_count != 0) {
      return std::unexpected(ValidationError{
          .function = 0,
          .instruction = 0,
          .message = "invalid static initializer function",
      });
    }
  }
  const auto identity = BindCodeIdentity(functions, isolate, code_identity);
  if (!identity) {
    return std::unexpected(identity.error());
  }
  if (globals != nullptr) {
    const auto& previous = globals->initializers;
    if (&globals->owner != &isolate.heap() || global_initializers.size() < previous.size() ||
        !std::ranges::equal(previous, std::span(global_initializers).first(previous.size()))) {
      return std::unexpected(ValidationError{
          .function = 0,
          .instruction = 0,
          .message = "incremental globals must preserve their isolate and initializer prefix",
      });
    }
    const auto extended = globals->Extend(std::move(global_initializers));
    if (!extended) {
      return std::unexpected(ValidationError{
          .function = 0,
          .instruction = 0,
          .message = "out of memory extending globals",
      });
    }
  } else if (!global_initializers.empty()) {
    globals = std::make_shared<GlobalStorage>(isolate.heap(), std::move(global_initializers));
    if (globals->storage.IsNull()) {
      return std::unexpected(ValidationError{
          .function = 0,
          .instruction = 0,
          .message = "out of memory allocating globals",
      });
    }
  }
  return Program(std::make_shared<const ProgramStorage>(ProgramStorage{
      .functions = std::move(functions),
      .code_identity = *identity,
      .isolate = &isolate,
      .globals = std::move(globals),
  }));
}

bool Program::HasCallable(const runtime::ClassLayout& layout) const noexcept {
  return layout.code_identity() == storage_->code_identity;
}

GlobalStorage::GlobalStorage(runtime::Heap& heap, std::vector<std::uint32_t> functions)
    : owner(heap),
      states(functions.size(), GlobalState::Uninitialized),
      initializers(std::move(functions)) {
  if (auto* object = heap.AllocateValueSlots(initializers.size())) {
    storage = runtime::Value::Object(object);
    values = runtime::Heap::ValueSlots(*object);
  }
}

std::expected<void, ExecutionStatus> GlobalStorage::Extend(std::vector<std::uint32_t> functions) {
  // 扩容发生在执行片段之间；旧 Program 与挂起帧共享同一个全局存储对象。
  const auto extended = owner.ExtendValueSlots(*storage.AsObject(), functions.size());
  if (!extended) {
    return extended;
  }
  values = runtime::Heap::ValueSlots(*storage.AsObject());
  states.resize(functions.size(), GlobalState::Uninitialized);
  initializers = std::move(functions);
  return {};
}

void GlobalStorage::Retain() noexcept {
  if (external_users++ == 0) {
    roots.Attach(owner, &storage, 1);
  }
}

void GlobalStorage::Release() noexcept {
  if (--external_users == 0) {
    roots.Detach();
  }
}

void GlobalStorage::Store(std::size_t index, runtime::Value value) noexcept {
  owner.SetValueSlot(*storage.AsObject(), index, value);
}

Program::Program(std::shared_ptr<const ProgramStorage> storage, Rooting rooting)
    : storage_(std::move(storage)), external_(rooting == Rooting::External) {
  Retain();
}

Program::~Program() {
  Release();
}
Program::Program(const Program& other)
    : Program(other.storage_, other.external_ ? Rooting::External : Rooting::Continuation) {}
Program& Program::operator=(const Program& other) {
  if (this != &other) {
    Release();
    storage_ = other.storage_;
    external_ = other.external_;
    Retain();
  }
  return *this;
}
Program::Program(Program&& other) noexcept
    : storage_(std::move(other.storage_)), external_(other.external_) {}
Program& Program::operator=(Program&& other) noexcept {
  if (this != &other) {
    Release();
    storage_ = std::move(other.storage_);
    external_ = other.external_;
  }
  return *this;
}
void Program::Retain() noexcept {
  if (storage_ != nullptr && external_ && storage_->globals != nullptr) {
    storage_->globals->Retain();
  }
}
void Program::Release() noexcept {
  if (storage_ != nullptr && external_ && storage_->globals != nullptr) {
    storage_->globals->Release();
  }
}
Program Program::Internal() const {
  return {storage_, Rooting::Continuation};
}

std::shared_ptr<GlobalStorage> Program::shared_globals() const noexcept {
  return storage_ != nullptr ? storage_->globals : nullptr;
}

std::expected<Program, ValidationError> Program::Load(
    std::span<const Function> functions,
    runtime::Isolate& isolate,
    std::vector<std::uint32_t> global_initializers) {
  std::vector<FunctionStorage> copied;
  absl::flat_hash_map<LayoutKey, std::shared_ptr<const LayoutTables>> layouts;
  copied.reserve(functions.size());
  for (const auto& function : functions) {
    FunctionStorage stored;
    stored.code.assign(function.code.begin(), function.code.end());
    stored.name = function.debug.name;
    stored.handlers.assign(function.handlers.begin(), function.handlers.end());
    if (!function.debug.source_paths.empty()) {
      stored.source_paths = std::make_shared<const std::vector<std::string>>(
          function.debug.source_paths.begin(), function.debug.source_paths.end());
    }
    stored.source_mappings.assign(function.debug.mappings.begin(), function.debug.mappings.end());
    if (!function.classes.empty() || !function.arrays.empty() || !function.collections.empty() ||
        !function.types.empty() || function.members != nullptr) {
      const LayoutKey key{
          .addresses =
              {
                  static_cast<const void*>(function.classes.data()),
                  static_cast<const void*>(function.arrays.data()),
                  static_cast<const void*>(function.collections.data()),
                  function.types.data(),
                  function.members,
              },
          .sizes =
              {
                  function.classes.size(),
                  function.arrays.size(),
                  function.collections.size(),
                  function.types.size(),
                  function.members != nullptr ? function.members->size() : 0,
              },
      };
      auto [entry, inserted] = layouts.try_emplace(key);
      if (inserted) {
        entry->second = std::make_shared<const LayoutTables>(
            std::vector(function.classes.begin(), function.classes.end()),
            std::vector(function.arrays.begin(), function.arrays.end()),
            std::vector(function.collections.begin(), function.collections.end()),
            std::vector(function.types.begin(), function.types.end()),
            function.members != nullptr ? *function.members : std::vector<MemberDefinition>{});
      }
      stored.layouts = entry->second;
    }
    stored.doubles.assign(function.doubles.begin(), function.doubles.end());
    stored.integers.assign(function.integers.begin(), function.integers.end());
    stored.register_count = function.register_count;
    if (function.native != nullptr) {
      stored.native = std::make_shared<const runtime::NativeFunction>(*function.native);
    }
    stored.parameter_count = function.parameter_count;
    stored.is_async = function.is_async;
    if (function.signature != nullptr) {
      stored.signature = *function.signature;
    }
    if (!function.strings.empty()) {
      auto strings = std::make_shared<StringConstants>(isolate.heap(), function.strings);
      stored.strings = std::move(strings);
    }
    copied.push_back(std::move(stored));
  }
  return Create(std::move(copied), isolate, std::move(global_initializers));
}

GlobalStorage* Program::globals() const noexcept {
  return storage_ != nullptr ? storage_->globals.get() : nullptr;
}

std::span<const FunctionStorage> Program::functions() const noexcept {
  return storage_ != nullptr ? std::span<const FunctionStorage>(storage_->functions)
                             : std::span<const FunctionStorage>();
}

FunctionHandle Program::FindFunction(std::string_view name) const noexcept {
  const auto stored = functions();
  const auto found = std::ranges::find_if(
      stored, [name](const auto& function) -> bool { return function.name == name; });
  if (found == stored.end()) {
    return {};
  }
  return {*this, static_cast<std::uint32_t>(found - stored.begin())};
}

bool FunctionHandle::IsValid() const noexcept {
  return program_.storage_ != nullptr;
}

bool Program::IsForIsolate(const runtime::Isolate& isolate) const noexcept {
  return storage_ != nullptr && storage_->isolate == &isolate;
}

std::uint16_t FunctionHandle::parameter_count() const noexcept {
  return program_.functions().subspan(index_).front().parameter_count;
}

bool FunctionHandle::is_async() const noexcept {
  return program_.functions().subspan(index_).front().is_async;
}

std::uint16_t FunctionHandle::register_count() const noexcept {
  return program_.functions().subspan(index_).front().register_count;
}

std::span<const Instruction> FunctionHandle::code() const noexcept {
  const auto& function = program_.storage_->functions.at(index_);
  return function.code;
}

}  // namespace vela::vm
