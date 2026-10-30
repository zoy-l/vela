#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/builtin.h"
#include "base/declaration.h"

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_file.h"

namespace vela::frontend {

struct TypeName final {
  enum class Kind : std::uint8_t { Named, Array, Nullable, Function, Generic };
  std::string name;
  SourceLocation location;
  Kind kind = Kind::Named;
  // 数组和可空类型有一个子类型，函数按参数顺序保存子类型，最后一项为返回类型。
  std::vector<TypeName> arguments;

  [[nodiscard]] std::string ToString() const;
};

// 名称绑定前的声明来源；键使用绑定后的符号名，值不受导入别名或模块排序影响。
// 泛型具体化保留模板来源及规范化实参，不从生成名称反向解析身份。
struct DeclarationOrigin final {
  struct Member final {
    DeclarationKind kind = DeclarationKind::Field;
    std::string name;
    bool has_initializer = false;
  };
  std::string module_uri;
  std::string name;
  DeclarationKind kind = DeclarationKind::Function;
  std::vector<TypeName> arguments;
  // 模板离开 AST 后仍保留声明结构；具体特化暂时不可达不代表这些声明已删除。
  std::vector<std::string> type_parameters;
  std::vector<Member> members;
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
  std::int64_t integer_value = 0;
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
};

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
};

struct Function final {
  enum class Role : std::uint8_t { Ordinary, InteractiveEntry, RetiredEntry };
  std::string name;
  TypeName return_type;
  std::vector<Parameter> parameters;
  std::unique_ptr<Statement> body;
  SourceLocation location;
  std::vector<std::string> type_parameters;
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
  SourceLocation location;
  std::vector<std::string> type_parameters;
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
  SourceLocation location;
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
    std::vector<std::string> type_parameters;
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
};

}  // namespace vela::frontend
