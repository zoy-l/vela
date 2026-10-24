#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

struct TypeName final {
  std::string name;
  SourceLocation location;
};

struct Expression;
struct Statement;

enum class BindingKind : std::uint8_t {
  Mutable,
  Constant,
};

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
};

struct Function final {
  std::string name;
  TypeName return_type;
  std::vector<Parameter> parameters;
  std::unique_ptr<Statement> body;
  SourceLocation location;
};

struct Enum final {
  std::string name;
  struct Member final {
    std::string name;
    SourceLocation location;
  };
  std::vector<Member> members;
  SourceLocation location;
};

struct Class final {
  std::string name;
  std::vector<Field> fields;
  std::vector<Function> methods;
  SourceLocation location;
};

struct Program final {
  struct Import final {
    std::string path;
    SourceLocation location;
  };
  std::vector<Import> imports;
  std::vector<std::string> source_paths;
  std::vector<Enum> enums;
  std::vector<Class> classes;
  std::vector<Function> functions;
};

}  // namespace vela::frontend
