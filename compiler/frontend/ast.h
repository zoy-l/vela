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

struct CallArgument final {
  std::string name;
  SourceLocation location;
  std::unique_ptr<Expression> value;
};

struct Expression final {
  enum class Kind {
    IntegerLiteral,
    FloatLiteral,
    StringLiteral,
    BooleanLiteral,
    Name,
    ShorthandMember,
    Unary,
    Binary,
    Call,
    Member,
  };

  Kind kind = Kind::IntegerLiteral;
  SourceLocation location;
  std::string text;
  std::int64_t integer_value = 0;
  double float_value = 0.0;
  bool boolean_value = false;
  std::unique_ptr<Expression> left;
  std::unique_ptr<Expression> right;
  std::unique_ptr<Expression> operand;
  std::unique_ptr<Expression> callee;
  std::unique_ptr<Expression> object;
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
  enum class Kind {
    VariableDeclaration,
    Return,
    Expression,
    Block,
  };

  Kind kind = Kind::Expression;
  SourceLocation location;
  TypeName type;
  std::string name;
  std::unique_ptr<Expression> expression;
  std::vector<std::unique_ptr<Statement>> statements;
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
  std::vector<std::string> members;
  SourceLocation location;
};

struct Class final {
  std::string name;
  std::vector<Field> fields;
  std::vector<Function> methods;
  SourceLocation location;
};

struct Program final {
  std::vector<Enum> enums;
  std::vector<Class> classes;
  std::vector<Function> functions;
};

}  // namespace vela::frontend
