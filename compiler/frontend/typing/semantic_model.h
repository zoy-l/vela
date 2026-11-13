#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>

#include "base/array_algorithm.h"
#include "base/async_operation.h"
#include "base/builtin.h"
#include "base/reflection.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/flow/control_flow.h"
#include "compiler/frontend/syntax/ast.h"

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
    // 泛型定义检查中的抽象参数；只出现在模板事实中，具体特化与后端不含此类型。
    Parameter,
  };

  Kind kind = Kind::Invalid;
  std::string_view name;
  bool nullable = false;
  // 含抽象类型参数的类型只供定义检查与工具使用，不参与执行布局或内建成员登记。
  bool is_abstract = false;
  // 对抽象参数的流分析事实：非空检查不把参数身份替换成上界。
  bool known_non_null = false;
  // 抽象参数显式借用声明节点；消费者通过身份与上界表读取语义，不解析显示名字。
  const TypeParameter* parameter = nullptr;
};

[[nodiscard]] TypeDescriptor MakeArrayType(const TypeDescriptor& element);
[[nodiscard]] TypeDescriptor MakeNullableType(TypeDescriptor type);
[[nodiscard]] std::string_view NonNullableTypeName(const TypeDescriptor& type) noexcept;

struct FunctionDescriptor final {
  enum class ParameterPresence : std::uint8_t { Required, Optional };
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
  std::optional<std::uint32_t> intrinsic_operation;
  Visibility visibility = Visibility::Public;
  MemberStorage storage = MemberStorage::Instance;
  bool is_getter = false;
  bool is_setter = false;
  std::vector<TypeDescriptor> type_arguments;
  std::optional<TypeDescriptor> setter_parameter_type;
  std::optional<TypeDescriptor> setter_return_type;
  // 所属执行声明；继承签名保留原始所有者，调用与接口实现不再重新搜索 AST。
  std::string member_owner;

  void AddParameter(std::string name,
                    TypeDescriptor type,
                    ParameterPresence presence = ParameterPresence::Required);
};

[[nodiscard]] TypeDescriptor MakeFunctionType(const FunctionDescriptor& signature);

struct GeneratedFunction final {
  std::unique_ptr<Function> declaration;
  FunctionDescriptor signature;
};

struct AsyncCallDescriptor final {
  AsyncOperation operation = AsyncOperation::Count;
  FunctionDescriptor signature;
  bool receiver = false;
  std::optional<TypeDescriptor> runtime_type = std::nullopt;
};

struct MemberReference final {
  std::string owner;
  std::string member;
};

enum class CallArgumentNames : std::uint8_t { Declared, Positional };

struct CallDescriptor final {
  FunctionDescriptor signature;
  CallArgumentNames argument_names = CallArgumentNames::Declared;
};

struct MethodTemplate final {
  std::string owner;
  const Function* declaration = nullptr;
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
  absl::node_hash_map<std::string, FunctionDescriptor> methods;
  // 方法模板按名字借用所属声明中的节点；调用点按接收者和推导实参特化具体方法。
  absl::flat_hash_map<std::string, MethodTemplate> method_templates;
  std::optional<std::string> base_class;
  FunctionDescriptor constructor;
  const Class* declaration = nullptr;
  absl::flat_hash_map<std::string, Field> static_fields;
  absl::flat_hash_set<std::string> interfaces;
  absl::flat_hash_map<std::pair<std::string, std::string>, MemberReference> interface_targets;
};

struct InterfaceDescriptor final {
  TypeDescriptor type;
  const Interface* declaration = nullptr;
  absl::node_hash_map<std::string, FunctionDescriptor> methods;
  absl::flat_hash_map<std::string, MethodTemplate> method_templates;
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

struct JSONDecodeParameter final {
  std::string name;
  std::uint32_t node = 0;
  bool optional = false;
};

// 规范类型身份对应唯一节点；递归边只保存节点编号，不展开或复制构造契约。
struct JSONDecodePlan final {
  TypeDescriptor type;
  std::optional<std::uint32_t> element;
  std::vector<JSONDecodeParameter> parameters;
};

struct JSONDecodeRequest final {
  TypeDescriptor type;
  SourceLocation location;
};

struct ReflectionMemberBinding final {
  ReflectionMemberKind kind = ReflectionMemberKind::Field;
  std::string name;
  std::vector<TypeDescriptor> type_arguments;
};

struct ReflectionBinding final {
  TypeDescriptor type;
  std::vector<ReflectionMemberBinding> members;
};

using reflection::AttributeScalar;

struct AttributeValue final {
  TypeDescriptor type;
  AttributeScalar value;
};

struct AttributeBinding final {
  const void* target = nullptr;
  TypeDescriptor type;
  std::vector<AttributeValue> fields;
};

// 以 AST 节点为键的检查事实。泛型模板按抽象参数检查时写入独立的一组，执行降级只读取具体声明的事实。
struct NodeFacts {
  absl::flat_hash_map<const Attribute*, TypeDescriptor> attribute_types;
  absl::node_hash_map<const Expression*, CallDescriptor> call_targets;
  std::vector<LambdaDescriptor> lambdas;
  absl::flat_hash_map<const Expression*, std::size_t> lambda_indices;
  absl::flat_hash_map<const void*, std::uint32_t> bindings;
  absl::flat_hash_map<const Expression*, TypeDescriptor> expression_types;
  absl::flat_hash_map<const Expression*, std::string> generic_applications;
  absl::flat_hash_map<const Expression*, TypeDescriptor> checked_conversions;
  absl::flat_hash_map<const Expression*, ConstantValue> case_values;
  absl::flat_hash_map<const Expression*, Builtin> builtin_calls;
  absl::flat_hash_map<const Expression*, AsyncCallDescriptor> async_calls;
  std::vector<GeneratedFunction> generated_functions;
  absl::flat_hash_map<const Expression*, std::size_t> builtin_method_values;
  absl::flat_hash_map<const Expression*, ArrayAlgorithm> array_algorithms;
  absl::flat_hash_map<const Statement*, TypeDescriptor> local_types;
  absl::flat_hash_map<const Statement*, StatementFlow> control_flow;
  absl::flat_hash_map<const Expression*, MemberReference> static_members;
  absl::flat_hash_map<const Expression*, MemberReference> instance_members;
  absl::flat_hash_map<const Expression*, MemberReference> interface_members;
  absl::flat_hash_map<const Expression*, MemberReference> super_members;
  absl::flat_hash_map<const Expression*, TypeDescriptor> type_operands;
  absl::flat_hash_map<const MatchArm*, TypeDescriptor> match_bindings;
  absl::flat_hash_map<const Statement::Catch*, TypeDescriptor> catch_types;
  absl::flat_hash_map<const Expression*, std::vector<std::uint32_t>> argument_order;
  absl::flat_hash_set<const Expression*> collection_constructions;
};

struct SemanticModel final : NodeFacts {
  absl::flat_hash_map<std::string, GlobalDescriptor> globals;
  absl::flat_hash_map<std::string, TypeDescriptor> types;
  absl::flat_hash_map<std::string, TypeDescriptor> array_elements;
  absl::node_hash_map<std::string, FunctionDescriptor> function_types;
  absl::flat_hash_set<std::uint32_t> captured_mutables;
  absl::flat_hash_map<std::string, std::int64_t> enum_values;
  absl::node_hash_map<std::string, ClassDescriptor> classes;
  absl::node_hash_map<std::string, FunctionDescriptor> functions;
  absl::node_hash_map<std::string, InterfaceDescriptor> interfaces;
  absl::flat_hash_set<std::string> explicit_super_constructors;
  absl::flat_hash_map<std::string, std::vector<TypeDescriptor>> generic_arguments;
  absl::flat_hash_map<TypeParameterId, TypeDescriptor> parameter_bounds;
  absl::flat_hash_map<std::string, TypeName> type_applications;
  absl::flat_hash_map<std::string, JSONDecodeRequest> json_decode_requests;
  std::vector<JSONDecodePlan> json_decode_plans;
  absl::flat_hash_map<std::string, std::uint32_t> json_decode_entries;
  std::vector<ReflectionBinding> reflection;
  std::vector<AttributeBinding> attributes;
  // 泛型模板定义检查的节点事实；节点属于 Program::templates 与 class/interface 的方法模板。
  NodeFacts definitions;
};

[[nodiscard]] Diagnostics BuildJSONDecodePlans(SemanticModel& model);

}  // namespace vela::frontend
