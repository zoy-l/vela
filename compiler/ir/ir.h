#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/layout_description.h"
#include "base/member.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_checker.h"

namespace vela::ir {

using RegisterId = std::uint32_t;
using ClassId = std::uint32_t;
using FieldId = std::uint32_t;
inline constexpr auto kInvalidArrayType = std::numeric_limits<std::uint32_t>::max();

using ::vela::Opcode;

struct Construction final {
  DeclarationId type;
  std::vector<DeclarationId> supplied;
  bool generated = true;
};

struct Instruction final {
  Opcode opcode = Opcode::LoadNull;
  RegisterId dst = 0;
  RegisterId lhs = 0;
  RegisterId rhs = 0;
  std::int64_t immediate = 0;
  std::uint32_t index = 0;
  frontend::SourceLocation location;
  std::optional<std::uint32_t> handler = std::nullopt;
  // 数组访问时的布局事实独立于寄存器表示；后续 dynamic 擦除不能改变元素存储形状。
  std::uint32_t array_type = kInvalidArrayType;
  std::optional<DeclarationId> field_declaration = std::nullopt;
  std::optional<DeclarationId> binding_receiver = std::nullopt;
  std::optional<Construction> construction = std::nullopt;
  bool completes_construction = false;
};

struct Class final {
  struct Field final {
    DeclarationId declaration;
    std::uint32_t type = 0;
    frontend::Visibility visibility = frontend::Visibility::Public;
    std::uint32_t initializer = kInvalidFunctionTarget;
  };
  enum class Kind : std::uint8_t { Instance, RuntimeError, DateParts, Enum, CaptureCell };
  std::string name;
  std::vector<std::string> field_order;
  std::vector<frontend::TypeDescriptor> field_types;
  std::optional<CallableTarget> callable;
  std::vector<std::uint32_t> interfaces;
  std::vector<std::uint32_t> dispatch_targets;
  Kind kind = Kind::Instance;
  std::string callable_type;
  DeclarationId declaration;
  std::vector<Field> fields;
  DeclarationId base_class;
};

struct Function final {
  struct Handler final {
    std::uint32_t target = 0;
  };
  std::string name;
  frontend::SourceLocation location;
  std::string owner_class;
  frontend::TypeDescriptor return_type;
  std::uint32_t parameter_count = 0;
  std::uint32_t register_count = 0;
  // lowering 的虚拟值类型；原生后端据此选择机器表示。
  std::vector<frontend::TypeDescriptor> value_types;
  std::vector<Instruction> instructions;
  std::vector<double> doubles;
  std::string native_symbol;
  std::vector<Handler> handlers;
  bool is_async = false;
  bool is_exported = false;
  bool is_generated_constructor = false;
  frontend::Visibility visibility = frontend::Visibility::Public;
  std::optional<CallSignature> signature = std::nullopt;
  FunctionIdentity identity;
  std::vector<DeclarationId> type_dependencies;
};

struct Declaration final {
  DeclarationId identity;
  DeclarationId owner;
  DeclarationKind kind = DeclarationKind::Function;
  std::string name;
  // 每个类型参数的规范契约文本，参数重命名不改变内容；用于判断热重载是否需要 Reload。
  std::vector<std::string> type_parameters;
  std::vector<std::string> enum_members;
  std::string generic_signature;
  DeclarationId definition;
};

struct Program final {
  std::vector<std::string> source_paths;
  std::vector<Class> classes;
  std::vector<Function> functions;
  std::vector<std::string> strings;
  std::vector<frontend::TypeDescriptor> array_types;
  std::vector<std::uint32_t> global_initializers;
  std::vector<frontend::TypeDescriptor> runtime_types;
  std::vector<frontend::TypeDescriptor> collection_types;
  std::vector<MemberDefinition> members;
  layout::Program layouts;
  std::uint64_t revision = 0;
  std::vector<std::string> source_texts;
  std::vector<Declaration> declarations;
  std::vector<DeclarationId> interface_declarations;
  std::vector<DeclarationId> global_declarations;
  std::vector<DeclarationId> selector_declarations;
  NativeProgramMode native_mode = NativeProgramMode::Closed;
  std::vector<std::uint32_t> global_flags;
};

}  // namespace vela::ir
