#include "base/layout_description.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include "base/type_kind.h"

namespace vela::layout {
namespace {

void AppendPart(std::string& output, std::string_view part) {
  output += std::to_string(part.size());
  output += ':';
  output += part;
}

}  // namespace

std::string_view ScalarTypeName(Type::Kind kind) noexcept {
  using Kind = Type::Kind;
  switch (kind) {
    case Kind::Null:
    case Kind::Never:
    case Kind::Dynamic:
    case Kind::Bool:
    case Kind::Int:
    case Kind::Double:
    case Kind::String:
    case Kind::MutableString:
    case Kind::ByteArray:
    case Kind::ByteData:
    case Kind::RegExp:
    case Kind::RegExpMatch:
    case Kind::Timer:
    case Kind::CancellationToken:
      return BuiltinTypeName(kind);
    case Kind::Class:
    case Kind::Enum:
    case Kind::Interface:
    case Kind::Array:
    case Kind::Set:
    case Kind::Map:
    case Kind::Function:
    case Kind::Future:
    case Kind::Completer:
      return {};
    case Kind::Invalid:
    case Kind::Void:
    case Kind::Parameter:
      std::unreachable();
  }
  std::unreachable();
}

namespace {

void AppendTypeKey(std::string& output, const Program& program, const Type& type);

void AppendSignatureKey(std::string& output, const Program& program, const Signature& signature) {
  output += std::to_string(signature.parameters.size());
  output += ':';
  for (const auto& parameter : signature.parameters) {
    output += parameter.optional ? '?' : '!';
    output += parameter.rest ? '*' : '.';
    AppendTypeKey(output, program, parameter.type);
  }
  if (signature.result) {
    AppendTypeKey(output, program, *signature.result);
  } else {
    output += BuiltinTypeName(Type::Kind::Void);
  }
  if (signature.narrowed_parameter) {
    output += "predicate:";
    AppendTypeKey(output, program, *signature.narrowed_parameter);
  }
  if (signature.false_narrowed_parameter) {
    output += "predicate:false:";
    AppendTypeKey(output, program, *signature.false_narrowed_parameter);
  }
}

void AppendTypeKey(std::string& output, const Program& program, const Type& type) {
  using Kind = Type::Kind;
  output += std::to_string(static_cast<std::uint8_t>(type.kind));
  output += type.nullable ? "?{" : "!{";
  switch (type.kind) {
    case Kind::Class:
    case Kind::Enum: {
      const auto& nominal = program.classes.at(type.index);
      AppendPart(output, nominal.native_key.empty() ? nominal.name : nominal.native_key);
      if (nominal.kind == Class::Kind::Host) {
        output += 'H';
      } else {
        output += nominal.kind == Class::Kind::Struct ? 'V' : 'S';
      }
      break;
    }
    case Kind::Interface:
      AppendPart(output, program.interfaces.at(type.index));
      break;
    case Kind::Array:
      AppendTypeKey(output, program, program.arrays.at(type.index).element);
      break;
    case Kind::Set:
    case Kind::Map: {
      const auto& collection = program.collections.at(type.index);
      AppendTypeKey(output, program, collection.key);
      if (collection.value) {
        AppendTypeKey(output, program, *collection.value);
      }
      break;
    }
    case Kind::Function:
    case Kind::Future:
    case Kind::Completer: {
      const auto& signature = program.signatures.at(type.index);
      AppendSignatureKey(output, program, signature);
      break;
    }
    default:
      AppendPart(output, ScalarTypeName(type.kind));
      break;
  }
  output += '}';
}

bool IsSameSignature(const Program& left_program,
                     const Signature& left,
                     const Program& right_program,
                     const Signature& right) {
  if (left.parameters.size() != right.parameters.size() ||
      left.result.has_value() != right.result.has_value()) {
    return false;
  }
  if (left.result && !IsSameType(left_program, *left.result, right_program, *right.result)) {
    return false;
  }
  const auto same_optional = [&](const std::optional<Type>& first,
                                 const std::optional<Type>& second) {
    return first.has_value() == second.has_value() &&
           (!first || IsSameType(left_program, *first, right_program, *second));
  };
  if (!same_optional(left.narrowed_parameter, right.narrowed_parameter) ||
      !same_optional(left.false_narrowed_parameter, right.false_narrowed_parameter)) {
    return false;
  }
  for (std::size_t index = 0; index < left.parameters.size(); ++index) {
    const auto& first = left.parameters[index];
    const auto& second = right.parameters[index];
    if (first.optional != second.optional || first.rest != second.rest ||
        !IsSameType(left_program, first.type, right_program, second.type)) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string NominalKey(const Program& program, const Nominal& nominal) {
  std::string result;
  AppendPart(result, nominal.module);
  AppendPart(result, nominal.name);
  result += std::to_string(nominal.arguments.size()) + ':';
  for (const auto& argument : nominal.arguments) {
    AppendPart(result, TypeKey(program, argument));
  }
  return result;
}

std::string TypeKey(const Program& program, const Type& type) {
  std::string result;
  AppendTypeKey(result, program, type);
  return result;
}

bool IsSameType(const Program& left_program,
                const Type& left,
                const Program& right_program,
                const Type& right) {
  using Kind = Type::Kind;
  if (left.kind != right.kind || left.nullable != right.nullable) {
    return false;
  }
  switch (left.kind) {
    case Kind::Class:
    case Kind::Enum: {
      const auto& first = left_program.classes.at(left.index);
      const auto& second = right_program.classes.at(right.index);
      const bool host = first.kind == Class::Kind::Host;
      return host == (second.kind == Class::Kind::Host) &&
             (first.kind == Class::Kind::Struct) == (second.kind == Class::Kind::Struct) &&
             (host ? first.native_key == second.native_key : first.name == second.name);
    }
    case Kind::Interface:
      return left_program.interfaces.at(left.index) == right_program.interfaces.at(right.index);
    case Kind::Array:
      return IsSameType(left_program, left_program.arrays.at(left.index).element, right_program,
                        right_program.arrays.at(right.index).element);
    case Kind::Set:
    case Kind::Map: {
      const auto& first = left_program.collections.at(left.index);
      const auto& second = right_program.collections.at(right.index);
      return first.value.has_value() == second.value.has_value() &&
             IsSameType(left_program, first.key, right_program, second.key) &&
             (!first.value || IsSameType(left_program, *first.value, right_program, *second.value));
    }
    case Kind::Function:
    case Kind::Future:
    case Kind::Completer:
      return IsSameSignature(left_program, left_program.signatures.at(left.index), right_program,
                             right_program.signatures.at(right.index));
    default:
      return true;
  }
}

Type ImportType(Program& target, const Program& source, const Type& type) {
  using Kind = Type::Kind;
  auto result = type;
  switch (type.kind) {
    case Kind::Class:
    case Kind::Enum: {
      const auto& nominal = source.classes.at(type.index);
      if (const auto found = source.nominals.find(nominal.name);
          found != source.nominals.end() && !target.nominals.contains(nominal.name)) {
        auto origin = found->second;
        for (auto& argument : origin.arguments) {
          argument = ImportType(target, source, argument);
        }
        target.nominals.emplace(nominal.name, std::move(origin));
      }
      result.index = static_cast<std::uint32_t>(target.classes.size());
      // 名义引用只导入身份；字段、分派和 callable 实现不属于类型契约。
      target.classes.push_back({
          .name = source.classes.at(type.index).name,
          .native_key = source.classes.at(type.index).native_key,
          .native_contract = {},
          .kind = source.classes.at(type.index).kind,
          .reference_fields = {},
          .callable = {},
          .interfaces = {},
          .value_slots = nominal.value_slots,
      });
      break;
    }
    case Kind::Interface: {
      const auto& name = source.interfaces.at(type.index);
      if (const auto found = source.nominals.find(name);
          found != source.nominals.end() && !target.nominals.contains(name)) {
        auto origin = found->second;
        for (auto& argument : origin.arguments) {
          argument = ImportType(target, source, argument);
        }
        target.nominals.emplace(name, std::move(origin));
      }
      result.index = static_cast<std::uint32_t>(target.interfaces.size());
      target.interfaces.push_back(source.interfaces.at(type.index));
      break;
    }
    case Kind::Array: {
      const auto element = ImportType(target, source, source.arrays.at(type.index).element);
      result.index = static_cast<std::uint32_t>(target.arrays.size());
      target.arrays.push_back({.name = TypeKey(source, type), .element = element});
      break;
    }
    case Kind::Set:
    case Kind::Map: {
      const auto& collection = source.collections.at(type.index);
      Collection imported{
          .name = TypeKey(source, type),
          .key = ImportType(target, source, collection.key),
      };
      if (collection.value) {
        imported.value = ImportType(target, source, *collection.value);
      }
      result.index = static_cast<std::uint32_t>(target.collections.size());
      target.collections.push_back(std::move(imported));
      break;
    }
    case Kind::Function:
    case Kind::Future:
    case Kind::Completer: {
      const auto& signature = source.signatures.at(type.index);
      Signature imported{.name = TypeKey(source, type), .parameters = {}};
      for (const auto& parameter : signature.parameters) {
        imported.parameters.push_back({
            .type = ImportType(target, source, parameter.type),
            .optional = parameter.optional,
            .rest = parameter.rest,
        });
      }
      if (signature.result) {
        imported.result = ImportType(target, source, *signature.result);
      }
      if (signature.narrowed_parameter) {
        imported.narrowed_parameter = ImportType(target, source, *signature.narrowed_parameter);
      }
      if (signature.false_narrowed_parameter) {
        imported.false_narrowed_parameter =
            ImportType(target, source, *signature.false_narrowed_parameter);
      }
      result.index = static_cast<std::uint32_t>(target.signatures.size());
      target.signatures.push_back(std::move(imported));
      break;
    }
    default:
      break;
  }
  return result;
}

}  // namespace vela::layout
