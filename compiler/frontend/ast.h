#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/builtin.h"
#include "base/declaration.h"

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_file.h"

namespace vela::frontend {

// 类型参数以所属泛型声明的规范路径和参数序号标识；同名而作用域不同的参数不是同一类型。
struct TypeParameterId final {
  std::string owner;
  std::uint32_t index = 0;

  bool operator==(const TypeParameterId&) const = default;
  template <class H>
  friend H AbslHashValue(H state, const TypeParameterId& identity) {
    return H::combine(std::move(state), identity.owner, identity.index);
  }
};

struct TypeName final {
  enum class Kind : std::uint8_t { Named, Array, Nullable, Function, Generic, Parameter };
  std::string name;
  SourceLocation location;
  Kind kind = Kind::Named;
  // 数组和可空类型有一个子类型，函数按参数顺序保存子类型，最后一项为返回类型。
  std::vector<TypeName> arguments;
  // 仅 Parameter 使用：模块绑定按词法作用域确定身份，name 只用于显示。
  TypeParameterId parameter;

  [[nodiscard]] std::string ToString() const;
};

// 声明中的类型参数；上界与引用它的类型使用同一参数身份。
struct TypeParameter final {
  std::string name;
  SourceLocation location;
  std::optional<TypeName> bound;
  TypeParameterId identity;
};

using TypeSubstitutions = absl::flat_hash_map<TypeParameterId, TypeName>;

// 参数具体化为已有可空类型或 dynamic 时，T? 不形成第二层可空包装。
[[nodiscard]] TypeName SubstituteTypeName(TypeName type, const TypeSubstitutions& substitutions);
[[nodiscard]] bool IsNullableTypeName(const TypeName& type);
[[nodiscard]] TypeName ParameterTypeName(const TypeParameter& parameter, SourceLocation location);
[[nodiscard]] bool HasTypeParameters(const TypeName& type);
[[nodiscard]] bool IsSameTypeConstructor(const TypeName& left, const TypeName& right);
// 与 ToString 相同的结构文本，但类型参数附带身份，作为特化与类型表的唯一键。
[[nodiscard]] std::string CanonicalTypeText(const TypeName& type);
[[nodiscard]] std::optional<std::string_view> CompoundAssignmentOperation(std::string_view text);

// 名称绑定前的声明来源；键使用绑定后的符号名，值不受导入别名或模块排序影响。
// 泛型具体化保留模板来源及规范化实参，不从生成名称反向解析身份。
struct DeclarationOrigin final {
  struct Member final {
    DeclarationKind kind = DeclarationKind::Field;
    std::string name;
    bool has_initializer = false;
    std::vector<std::string> type_parameters;
    std::string generic_signature;
  };
  std::string module_uri;
  std::string name;
  DeclarationKind kind = DeclarationKind::Function;
  std::vector<TypeName> arguments;
  // 模板离开 AST 后仍保留声明结构；具体特化暂时不可达不代表这些声明已删除。
  std::vector<TypeParameter> type_parameters;
  std::vector<Member> members;
  std::string generic_signature;
};

struct Expression;
struct Statement;
struct Function;

struct MatchArm final {
  enum class Kind : std::uint8_t { Constant, Binding, Wildcard };
  Kind kind = Kind::Constant;
  SourceLocation location;
  std::string binding;
  std::unique_ptr<Expression> pattern;
  std::unique_ptr<Expression> value;
};

enum class BindingKind : std::uint8_t {
  Mutable,
  Constant,
};

enum class Visibility : std::uint8_t { Public, Private };
enum class MemberStorage : std::uint8_t { Instance, Static };
enum class ConstructorKind : std::uint8_t { Generated, Explicit };
enum class ExportKind : std::uint8_t { Local, Exported };

struct CallArgument final {
  std::string name;
  SourceLocation location;
  std::unique_ptr<Expression> value;
};

struct Expression final {
  enum class Kind : std::uint8_t {
    IntegerLiteral,
    DoubleLiteral,
    StringLiteral,
    StringInterpolation,
    BooleanLiteral,
    NullLiteral,
    Name,
    Unary,
    Binary,
    Call,
    Member,
    ArrayLiteral,
    Index,
    Lambda,
    TypeTest,
    TypeCast,
    Match,
    Await,
    Conditional,
  };

  Kind kind = Kind::IntegerLiteral;
  SourceLocation location;
  std::string text;
  bool null_safe = false;
  bool postfix = false;
  double double_value = 0.0;
  bool boolean_value = false;
  std::unique_ptr<Expression> left;
  std::unique_ptr<Expression> right;
  std::unique_ptr<Expression> operand;
  std::unique_ptr<Expression> callee;
  std::unique_ptr<Expression> object;
  std::unique_ptr<Expression> index;
  std::vector<std::unique_ptr<Expression>> elements;
  std::vector<CallArgument> arguments;
  std::unique_ptr<Function> function;
  std::vector<TypeName> type_arguments;
  TypeName type;
  std::vector<MatchArm> arms;

  // 文本语法由 lexer/parser 保证，各阶段共用有符号范围检查。
  [[nodiscard]] std::optional<std::int64_t> IntegerValue() const noexcept {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), std::to_address(text.end()), value);
    return parsed.ec == std::errc() ? std::optional(value) : std::nullopt;
  }
};

// 仅枚举六个可选操作数；列表、lambda、match 的遍历和作用域由访问者处理。
template <class Node>
[[nodiscard]] auto ExpressionOperands(Node& expression) noexcept {
  return std::array{
      expression.left.get(),   expression.right.get(),  expression.operand.get(),
      expression.callee.get(), expression.object.get(), expression.index.get(),
  };
}

struct Parameter final {
  TypeName type;
  std::string name;
  SourceLocation location;
};

struct Field final {
  TypeName type;
  std::string name;
  SourceLocation location;
  Visibility visibility = Visibility::Public;
  MemberStorage storage = MemberStorage::Instance;
  std::unique_ptr<Expression> initializer;
};

struct Statement final {
  enum class Kind : std::uint8_t {
    VariableDeclaration,
    Return,
    Expression,
    Block,
    If,
    While,
    For,
    ForEach,
    Break,
    Continue,
    Switch,
    Try,
    Throw,
  };

  Kind kind = Kind::Expression;
  SourceLocation location;
  TypeName type;
  BindingKind binding = BindingKind::Mutable;
  std::string name;
  std::unique_ptr<Expression> expression;
  std::unique_ptr<Expression> condition;
  std::unique_ptr<Statement> then_branch;
  std::unique_ptr<Statement> else_branch;
  std::unique_ptr<Statement> initializer;
  std::unique_ptr<Expression> increment;
  std::vector<std::unique_ptr<Statement>> statements;
  // switch 使用有序分支，空分支代表与后续分支共享入口；空标签代表 default。
  struct Case final {
    SourceLocation location;
    std::unique_ptr<Expression> label;
    std::unique_ptr<Statement> body;
  };
  std::vector<Case> cases;
  struct Catch final {
    SourceLocation location;
    std::string name;
    TypeName type;
    std::unique_ptr<Statement> body;
  };
  std::vector<Catch> catches;
  std::unique_ptr<Statement> finally_branch;
  // 仅交互入口的末尾表达式回显；普通语法不产生此标记。
  bool echo = false;
  std::unique_ptr<Expression> iterable;
  std::string second_name;
  bool destructuring = false;

  [[nodiscard]] bool IntroducesScope() const noexcept {
    return kind == Kind::Block || kind == Kind::For || kind == Kind::ForEach;
  }
};

struct Function final {
  enum class Role : std::uint8_t { Ordinary, InteractiveEntry, RetiredEntry };
  struct Application final {
    std::string name;
    std::vector<TypeName> arguments;
  };
  std::string name;
  TypeName return_type;
  std::vector<Parameter> parameters;
  std::unique_ptr<Statement> body;
  SourceLocation location;
  std::vector<TypeParameter> type_parameters;
  // 方法应用保留模板名及已验证实参；持久身份和工具直接读取，不从执行名称反向解析。
  std::optional<Application> application;
  Visibility visibility = Visibility::Public;
  MemberStorage storage = MemberStorage::Instance;
  ExportKind export_kind = ExportKind::Local;
  std::string native_symbol;
  // 仅标准声明生产者设置；源码不能声明或覆盖 intrinsic。
  std::optional<Builtin> intrinsic;
  // SDK 声明生产者登记可省略的参数；省略值沿现有调用契约以 null 传递。
  std::vector<std::string> optional_parameters;
  Role role = Role::Ordinary;
  bool is_async = false;
  bool is_override = false;
  bool is_getter = false;
  bool is_setter = false;
};

struct Enum final {
  std::string name;
  struct Member final {
    std::string name;
    SourceLocation location;
  };
  std::vector<Member> members;
  SourceLocation location;
  ExportKind export_kind = ExportKind::Local;
};

struct Class final {
  std::string name;
  std::vector<Field> fields;
  std::vector<Function> methods;
  // 声明了自身类型参数的方法不进入执行用方法表；调用点的具体应用在检查结束时追加到 methods。
  std::vector<Function> method_templates;
  SourceLocation location;
  std::vector<TypeParameter> type_parameters;
  std::unique_ptr<Function> constructor;
  ConstructorKind constructor_kind = ConstructorKind::Generated;
  std::vector<TypeName> interfaces;
  std::optional<TypeName> base_class;
  ExportKind export_kind = ExportKind::Local;

  [[nodiscard]] bool IsConstructorField(const Field& field) const {
    return constructor_kind == ConstructorKind::Generated &&
           field.storage == MemberStorage::Instance && field.visibility == Visibility::Public;
  }
};

struct Interface final {
  std::string name;
  std::vector<Function> methods;
  std::vector<Function> method_templates;
  SourceLocation location;
  std::vector<TypeParameter> type_parameters;
  ExportKind export_kind = ExportKind::Local;
};

struct Global final {
  std::string name;
  TypeName type;
  std::unique_ptr<Expression> initializer;
  SourceLocation location;
  BindingKind binding = BindingKind::Mutable;
  ExportKind export_kind = ExportKind::Local;
};

struct Program final {
  struct TypeAlias final {
    std::string name;
    TypeName target;
    SourceLocation location;
    std::vector<TypeParameter> type_parameters;
    ExportKind export_kind = ExportKind::Local;
  };
  struct Import final {
    std::string path;
    SourceLocation location;
    std::string alias;
  };
  std::vector<Import> imports;
  std::vector<SourceFile> sources;
  std::vector<FileResolution> file_resolutions;
  std::vector<std::string> standard_names;
  std::vector<Enum> enums;
  std::vector<Class> classes;
  std::vector<Function> functions;
  std::vector<TypeAlias> aliases;
  std::vector<Interface> interfaces;
  std::vector<Global> globals;
  absl::flat_hash_map<std::string, DeclarationOrigin> origins;
  // 特化把泛型模板移出执行用声明；模板在特化初始化后不再增删，定义检查事实与工具共用这些节点。
  struct Templates final {
    std::vector<Function> functions;
    std::vector<Class> classes;
    std::vector<Interface> interfaces;
    std::vector<TypeAlias> aliases;
    // 实参含类型参数的抽象应用只提供成员签名，不进入执行用声明；检查期间按需追加。
    std::vector<std::unique_ptr<Class>> class_applications;
    std::vector<std::unique_ptr<Interface>> interface_applications;
  };
  Templates templates;
};

}  // namespace vela::frontend
