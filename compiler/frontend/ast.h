#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
enum class FieldRole : std::uint8_t { Ordinary, Parameter };
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
  };

  Kind kind = Kind::IntegerLiteral;
  SourceLocation location;
  std::string text;
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
  FieldRole role = FieldRole::Ordinary;
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
  Role role = Role::Ordinary;
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
  ExportKind export_kind = ExportKind::Local;
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
  std::vector<Enum> enums;
  std::vector<Class> classes;
  std::vector<Function> functions;
  std::vector<TypeAlias> aliases;
  std::vector<Interface> interfaces;
  std::vector<Global> globals;
};

}  // namespace vela::frontend
