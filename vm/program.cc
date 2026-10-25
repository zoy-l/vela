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

#include "base/builtin.h"
#include "base/callable.h"
#include "base/execution_limits.h"
#include "base/utf8.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "vm/bytecode.h"

namespace vela::vm {
namespace {

struct LayoutKey final {
  std::array<const void*, 4> addresses;
  std::array<std::size_t, 4> sizes;

  auto operator==(const LayoutKey&) const -> bool = default;

  template <typename H>
  friend auto AbslHashValue(H hash, const LayoutKey& key) -> H {
    return H::combine(std::move(hash), key.addresses, key.sizes);
  }
};

auto IsRegister(std::uint8_t index, const Function& function) noexcept -> bool {
  return index < function.register_count;
}

auto IsValidInstruction(Instruction instruction,
                        const Function& function,
                        std::span<const FunctionStorage> functions,
                        std::size_t global_count) noexcept -> bool {
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
    case OpCode::NativeCall:
      return function.native != nullptr && IsRegister(operand_a, function) &&
             operand_c == function.native->parameters().size() &&
             static_cast<std::size_t>(operand_b) + operand_c <= function.register_count;
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
    case OpCode::StringInterpolate:
      return IsRegister(operand_a, function) &&
             static_cast<std::size_t>(operand_b) + operand_c <= function.register_count;
    case OpCode::Jump:
      return true;
    case OpCode::JumpIfFalse:
      return IsRegister(operand_a, function);
    case OpCode::NewInstance:
      return IsRegister(operand_a, function) && operand_b < function.classes.size() &&
             function.classes.subspan(operand_b).front() != nullptr;
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

auto ValidateClassLayout(const runtime::ClassLayout* layout,
                         std::span<const FunctionStorage> storage,
                         std::size_t index) -> std::expected<void, ValidationError> {
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
             (callable.mode == CallableMode::BoundMethod && layout->field_count() == 0)) {
    return std::unexpected(
        ValidationError{.function = index, .instruction = 0, .message = "invalid callable layout"});
  }
  return {};
}

auto ValidateFunctionCode(std::size_t index,
                          const Function& function,
                          std::span<const FunctionStorage> storage,
                          std::size_t global_count) -> std::expected<void, ValidationError> {
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
    if (mapping.instruction >= function.code.size() ||
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
      if (target < 0 || static_cast<std::uint64_t>(target) >= function.code.size()) {
        return std::unexpected(ValidationError{
            .function = index,
            .instruction = position,
            .message = "jump target is outside the function",
        });
      }
    }
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

auto ValidateFunctions(std::span<const FunctionStorage> storage,
                       std::span<const std::uint32_t> initializers)
    -> std::expected<void, ValidationError> {
  absl::flat_hash_set<const runtime::ClassLayout*> validated_layouts;
  absl::flat_hash_set<const LayoutTables*> validated_tables;
  absl::flat_hash_set<const std::vector<std::string>*> validated_paths;
  for (std::size_t index = 0; index < storage.size(); ++index) {
    const auto& stored = storage.subspan(index).front();
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
    if (function.code.empty() || function.register_count == 0 ||
        function.register_count > kMaxFrameRegisters ||
        function.parameter_count > function.register_count) {
      return std::unexpected(ValidationError{
          .function = index,
          .instruction = 0,
          .message = "invalid function metadata",
      });
    }
    const auto* table = stored.layouts.get();
    const auto classes = validated_tables.insert(table).second
                             ? function.classes
                             : std::span<runtime::ClassLayout* const>();
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
  return {};
}

}  // namespace

LayoutTables::LayoutTables(std::vector<runtime::ClassLayout*> classes,
                           std::vector<const runtime::ArrayLayout*> arrays,
                           std::vector<const runtime::CollectionLayout*> collections,
                           std::vector<runtime::TypeConstraint> types)
    : classes_(std::move(classes)),
      arrays_(std::move(arrays)),
      collections_(std::move(collections)),
      types_(std::move(types)) {}

auto FunctionStorage::View() const noexcept -> Function {
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
  };
}

auto Program::Create(std::vector<FunctionStorage> functions,
                     runtime::Isolate& isolate,
                     std::vector<std::uint32_t> global_initializers)
    -> std::expected<Program, ValidationError> {
  const auto valid = ValidateFunctions(functions, global_initializers);
  if (!valid) {
    return std::unexpected(valid.error());
  }
  for (const auto initializer : global_initializers) {
    if (initializer >= functions.size() || functions.at(initializer).parameter_count != 0) {
      return std::unexpected(ValidationError{
          .function = 0,
          .instruction = 0,
          .message = "invalid static initializer function",
      });
    }
  }
  auto globals = global_initializers.empty() ? nullptr
                                             : std::make_shared<GlobalStorage>(
                                                   isolate.heap(), std::move(global_initializers));
  return Program(std::make_shared<const ProgramStorage>(ProgramStorage{
      .functions = std::move(functions),
      .isolate = &isolate,
      .globals = std::move(globals),
  }));
}

auto Program::Load(std::span<const Function> functions,
                   runtime::Isolate& isolate,
                   std::vector<std::uint32_t> global_initializers)
    -> std::expected<Program, ValidationError> {
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
        !function.types.empty()) {
      const LayoutKey key{
          .addresses =
              {
                  static_cast<const void*>(function.classes.data()),
                  static_cast<const void*>(function.arrays.data()),
                  static_cast<const void*>(function.collections.data()),
                  function.types.data(),
              },
          .sizes =
              {
                  function.classes.size(),
                  function.arrays.size(),
                  function.collections.size(),
                  function.types.size(),
              },
      };
      auto [entry, inserted] = layouts.try_emplace(key);
      if (inserted) {
        entry->second = std::make_shared<const LayoutTables>(
            std::vector(function.classes.begin(), function.classes.end()),
            std::vector(function.arrays.begin(), function.arrays.end()),
            std::vector(function.collections.begin(), function.collections.end()),
            std::vector(function.types.begin(), function.types.end()));
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
    if (!function.strings.empty()) {
      auto strings = std::make_shared<StringConstants>(isolate.heap(), function.strings);
      stored.strings = std::move(strings);
    }
    copied.push_back(std::move(stored));
  }
  return Create(std::move(copied), isolate, std::move(global_initializers));
}

auto Program::globals() const noexcept -> GlobalStorage* {
  return storage_ != nullptr ? storage_->globals.get() : nullptr;
}

auto Program::functions() const noexcept -> std::span<const FunctionStorage> {
  return storage_ != nullptr ? std::span<const FunctionStorage>(storage_->functions)
                             : std::span<const FunctionStorage>();
}

auto Program::FindFunction(std::string_view name) const noexcept -> FunctionHandle {
  const auto stored = functions();
  const auto found = std::ranges::find_if(
      stored, [name](const auto& function) -> bool { return function.name == name; });
  if (found == stored.end()) {
    return {};
  }
  return {*this, static_cast<std::uint32_t>(found - stored.begin())};
}

auto FunctionHandle::IsValid() const noexcept -> bool {
  return program_.storage_ != nullptr;
}

auto Program::IsForIsolate(const runtime::Isolate& isolate) const noexcept -> bool {
  return storage_ != nullptr && storage_->isolate == &isolate;
}

auto FunctionHandle::parameter_count() const noexcept -> std::uint16_t {
  return program_.functions().subspan(index_).front().parameter_count;
}

auto FunctionHandle::register_count() const noexcept -> std::uint16_t {
  return program_.functions().subspan(index_).front().register_count;
}

auto FunctionHandle::code() const noexcept -> std::span<const Instruction> {
  const auto& function = program_.storage_->functions.at(index_);
  return function.code;
}

auto FunctionHandle::doubles() const noexcept -> std::span<const double> {
  const auto& function = program_.storage_->functions.at(index_);
  return function.doubles;
}

}  // namespace vela::vm
