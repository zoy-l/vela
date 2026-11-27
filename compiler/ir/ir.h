#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/json_decode.h"
#include "base/layout_description.h"
#include "base/member.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "base/reflection.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"

namespace vela::ir {

inline constexpr const char* kCaptureCellName = "$Cell";

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

struct StructReceiver final {
  enum class Kind : std::uint8_t { Local, Global, Field, Index };
  Kind kind = Kind::Local;
  RegisterId receiver = 0;
  RegisterId index = 0;
  std::uint32_t slot = 0;
  std::optional<DeclarationId> declaration;
  frontend::TypeDescriptor type;
  std::vector<std::uint32_t> fields;
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
  // 封闭 AOT 在裁剪后证明的描述符来源，索引属于本程序的 reflection_members。
  std::optional<std::uint32_t> reflection_member = std::nullopt;
  std::optional<Construction> construction = std::nullopt;
  bool completes_construction = false;
  bool no_normal_return = false;
  // NewInstance/GetField/SetField 对 struct 寄存器操作内联值，不访问堆对象身份。
  bool inline_value = false;
  // 稀少的借用调用独立持有不可变位置描述，普通指令不承担其完整存储成本。
  std::shared_ptr<const StructReceiver> struct_receiver = nullptr;
};

struct Class final {
  struct Field final {
    DeclarationId declaration;
    std::uint32_t type = 0;
    frontend::Visibility visibility = frontend::Visibility::Public;
    frontend::BindingKind binding = frontend::BindingKind::Mutable;
    std::uint32_t initializer = kInvalidFunctionTarget;
  };
  enum class Kind : std::uint8_t {
    Instance,
    RuntimeError,
    DateParts,
    Enum,
    CaptureCell,
    Host,
    Metadata,
    ReflectionIdentity,
    Struct,
  };
  std::string name;
  std::vector<frontend::TypeDescriptor> field_types;
  // 值布局由语义检查唯一生成；逻辑字段与扁平槽的对应关系贯穿全部原生后端。
  std::shared_ptr<frontend::StructLayout> value_layout = nullptr;
  std::optional<CallableTarget> callable;
  std::vector<std::uint32_t> interfaces;
  std::vector<std::uint32_t> dispatch_targets;
  Kind kind = Kind::Instance;
  std::string native_key;
  std::string native_contract;
  std::optional<CallSignature> callable_type = std::nullopt;
  DeclarationId declaration;
  std::vector<Field> fields;
  DeclarationId base_class;
  std::uint32_t constructor = kInvalidFunctionTarget;
  std::vector<std::string> enum_members;
};

struct Function final {
  struct Handler final {
    std::uint32_t target = 0;
  };
  std::string name;
  frontend::SourceLocation location;
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
  // 同步值方法借用接收位置；构造和异步状态机分别拥有自己的值。
  bool struct_receiver = false;
  frontend::Visibility visibility = frontend::Visibility::Public;
  std::optional<CallSignature> signature = std::nullopt;
  FunctionIdentity identity;
  std::vector<DeclarationId> type_dependencies;
  std::optional<std::uint32_t> json_decode_plan = std::nullopt;
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

struct JSONDecodeParameter final {
  std::string name;
  std::uint32_t node = 0;
  bool optional = false;
};

struct JSONDecodePlan final {
  std::string name;
  JSONDecodeKind kind = JSONDecodeKind::Dynamic;
  std::uint32_t type = 0;
  std::uint32_t element = 0;
  std::uint32_t constructor = kInvalidFunctionTarget;
  std::vector<JSONDecodeParameter> parameters;
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
  std::vector<JSONDecodePlan> json_decode_plans;
  std::vector<reflection::Type> reflection_types;
  std::vector<reflection::Member> reflection_members;
  std::vector<reflection::Attribute> attributes;
};

[[nodiscard]] std::vector<std::uint32_t> JSONDecodeClosure(const Program& program,
                                                           std::uint32_t root);

}  // namespace vela::ir
