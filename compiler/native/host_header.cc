#include "compiler/native/host_header.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/layout_description.h"
#include "base/member.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {
namespace {

std::string Tag(std::string_view name, std::string_view name_space) {
  return std::string(name_space) + "::Type_" + HostIdentifier(name) + "Tag";
}

std::string Literal(std::string_view text) {
  constexpr std::uint8_t kFirstPrintableAscii = 0x20;
  constexpr std::uint8_t kDeleteAscii = 0x7f;
  constexpr unsigned kOctalDigitBits = 3;
  constexpr unsigned kOctalDigitMask = (1U << kOctalDigitBits) - 1U;
  std::string result = "\"";
  for (const auto character : text) {
    if (character == '\\' || character == '"') {
      result += '\\';
    }
    const auto byte = static_cast<std::uint8_t>(character);
    if (byte < kFirstPrintableAscii || byte == kDeleteAscii) {
      result += '\\';
      result += static_cast<char>('0' + ((byte >> (2U * kOctalDigitBits)) & kOctalDigitMask));
      result += static_cast<char>('0' + ((byte >> kOctalDigitBits) & kOctalDigitMask));
      result += static_cast<char>('0' + (byte & kOctalDigitMask));
      continue;
    }
    result += character;
  }
  return result + '"';
}

bool IsHostNamespace(std::string_view name) {
  constexpr auto kKeywords = std::to_array<std::string_view>({
      "alignas",       "alignof",     "and",
      "and_eq",        "asm",         "auto",
      "bitand",        "bitor",       "bool",
      "break",         "case",        "catch",
      "char",          "char8_t",     "char16_t",
      "char32_t",      "class",       "compl",
      "concept",       "const",       "consteval",
      "constexpr",     "constinit",   "const_cast",
      "continue",      "co_await",    "co_return",
      "co_yield",      "decltype",    "default",
      "delete",        "do",          "double",
      "dynamic_cast",  "else",        "enum",
      "explicit",      "export",      "extern",
      "false",         "float",       "for",
      "friend",        "goto",        "if",
      "inline",        "int",         "long",
      "mutable",       "namespace",   "new",
      "noexcept",      "not",         "not_eq",
      "nullptr",       "operator",    "or",
      "or_eq",         "private",     "protected",
      "public",        "register",    "reinterpret_cast",
      "requires",      "return",      "short",
      "signed",        "sizeof",      "static",
      "static_assert", "static_cast", "struct",
      "switch",        "template",    "this",
      "thread_local",  "throw",       "true",
      "try",           "typedef",     "typeid",
      "typename",      "union",       "unsigned",
      "using",         "virtual",     "void",
      "volatile",      "wchar_t",     "while",
      "xor",           "xor_eq",
  });
  const auto letter = [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
  };
  while (!name.empty()) {
    const auto separator = name.find("::");
    const auto part = name.substr(0, separator);
    if (part.empty() || !letter(part.front()) || std::ranges::contains(kKeywords, part) ||
        part.contains("__") || !std::ranges::all_of(part, [&](char value) {
          return letter(value) || value == '_' || (value >= '0' && value <= '9');
        })) {
      return false;
    }
    if (separator == std::string_view::npos) {
      return true;
    }
    name.remove_prefix(separator + 2);
  }
  return false;
}

bool IsSourceClass(const ir::Class& type) {
  return !type.callable && type.kind != ir::Class::Kind::CaptureCell;
}

bool IsExported(const layout::Program& program, std::string_view name) {
  const auto found = program.nominals.find(name);
  return found != program.nominals.end() && found->second.exported;
}

std::optional<layout::Type> MemberType(const ir::Program& program, const MemberDefinition& member) {
  if (member.type) {
    return program.layouts.types.at(*member.type);
  }
  if (member.kind == MemberKind::Method) {
    const auto& callable = program.layouts.classes.at(member.target);
    if (callable.callable_type) {
      return layout::Type{.kind = layout::Type::Kind::Callable, .index = *callable.callable_type};
    }
  }
  if (member.kind == MemberKind::Property && member.target != kInvalidFunctionTarget) {
    const auto& getter = program.functions.at(member.target);
    if (!getter.signature) {
      std::unreachable();
    }
    const auto& signature = *getter.signature;
    if (signature.result) {
      return program.layouts.types.at(*signature.result);
    }
  }
  return std::nullopt;
}

std::expected<void, std::string> EmitConstructor(std::ostringstream& output,
                                                 const ir::Program& program,
                                                 std::string_view name_space,
                                                 const ir::Function& constructor,
                                                 std::string_view logical) {
  if (!constructor.signature) {
    std::unreachable();
  }
  const auto& signature = *constructor.signature;
  const auto count = signature.parameters.size() / 2;
  std::vector<std::string> parameters;
  for (std::size_t index = 0; index < count; ++index) {
    const auto parameter = HostLogicalType(
        program.layouts, program.layouts.types.at(signature.parameters[index].type), name_space);
    if (!parameter) {
      return std::unexpected(parameter.error());
    }
    parameters.push_back(*parameter);
  }
  output << "  static vela::Result<vela::HostValue<" << logical << ">> Construct(const vela::Type<"
         << logical << ">& type, vela::HandleScope& scope";
  const auto emit_parameter = [&](std::size_t index) {
    const auto optional = signature.parameters[index].optional;
    output << ", " << (optional ? "std::optional<" : "") << "vela::HostValue<" << parameters[index]
           << ">" << (optional ? ">" : "") << " argument" << index
           << (optional ? " = std::nullopt" : "");
  };
  for (std::size_t index = 0; index < count; ++index) {
    if (!signature.parameters[index].optional) {
      emit_parameter(index);
    }
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (signature.parameters[index].optional) {
      emit_parameter(index);
    }
  }
  output << ") { return type.Construct(scope";
  for (std::size_t index = 0; index < count; ++index) {
    output << ", vela::NamedArgument<" << parameters[index]
           << ">{.name = " << Literal(signature.parameters[index].name) << ", .value = argument"
           << index << '}';
  }
  output << "); }\n";
  return {};
}

std::expected<void, std::string> EmitMember(std::ostringstream& output,
                                            const ir::Program& program,
                                            std::string_view name_space,
                                            const MemberDefinition& member,
                                            std::string_view logical) {
  const auto member_type = MemberType(program, member);
  if (member_type) {
    const auto result = HostLogicalType(program.layouts, *member_type, name_space);
    if (!result) {
      return std::unexpected("member " + member.name + ": " + result.error());
    }
    output << "  static vela::Result<vela::HostValue<" << *result << ">> Get_"
           << HostIdentifier(member.name) << "(const vela::Type<" << logical
           << ">& type, vela::HandleScope& scope, vela::HostValue<" << logical
           << "> receiver) { return type.Get<" << *result << ">(scope, receiver, "
           << Literal(member.name) << "); }\n";
    if (member.kind == MemberKind::Method) {
      output << "  template <class... Args> static auto Call_" << HostIdentifier(member.name)
             << "(const vela::Type<" << logical
             << ">& type, vela::HandleScope& scope, vela::HostValue<" << logical
             << "> receiver, Args&&... arguments) { auto method = Get_"
             << HostIdentifier(member.name)
             << "(type, scope, receiver); using Result = decltype(method->Call(scope, "
                "std::forward<Args>(arguments)...)); "
                "if (!method) { return Result(std::unexpected(std::move(method.error()))); } "
                "return method->Call(scope, std::forward<Args>(arguments)...); }\n";
    }
  }
  std::optional<layout::Type> write_type;
  if (member.kind == MemberKind::Field) {
    write_type = member_type;
  } else if (member.setter) {
    const auto& setter = program.functions.at(*member.setter);
    if (!setter.signature) {
      std::unreachable();
    }
    const auto& signature = *setter.signature;
    write_type = program.layouts.types.at(signature.parameters.back().type);
  }
  if (write_type) {
    const auto value = HostLogicalType(program.layouts, *write_type, name_space);
    if (!value) {
      return std::unexpected(value.error());
    }
    output << "  static vela::Result<void> Set_" << HostIdentifier(member.name)
           << "(const vela::Type<" << logical
           << ">& type, vela::HandleScope& scope, vela::HostValue<" << logical
           << "> receiver, vela::HostValue<" << *value << "> value) { return type.Set<" << *value
           << ">(scope, receiver, " << Literal(member.name) << ", value); }\n";
  }
  return {};
}

std::expected<void, std::string> EmitMembers(std::ostringstream& output,
                                             const ir::Program& program,
                                             std::string_view name_space,
                                             layout::Type receiver,
                                             std::string_view name,
                                             std::span<const MemberDefinition* const> members) {
  const auto logical = HostLogicalType(program.layouts, receiver, name_space);
  if (!logical) {
    return std::unexpected(logical.error());
  }
  output << "struct Type_" << HostIdentifier(name) << "Members final {\n";
  if (receiver.kind == layout::Type::Kind::Enum) {
    for (const auto& member : program.classes.at(receiver.index).enum_members) {
      output << "  static vela::Result<vela::HostValue<" << *logical << ">> Member_"
             << HostIdentifier(member) << "(const vela::Type<" << *logical
             << ">& type, vela::HandleScope& scope) { return type.Member(scope, " << Literal(member)
             << "); }\n";
    }
  } else if (receiver.kind == layout::Type::Kind::Instance) {
    const auto constructor = program.classes.at(receiver.index).constructor;
    if (constructor != kInvalidFunctionTarget) {
      auto emitted =
          EmitConstructor(output, program, name_space, program.functions.at(constructor), *logical);
      if (!emitted) {
        return emitted;
      }
    }
  }
  for (const auto* member : members) {
    auto emitted = EmitMember(output, program, name_space, *member, *logical);
    if (!emitted) {
      return emitted;
    }
  }
  output << "};\n";
  return {};
}

std::expected<void, std::string> EmitEntries(std::ostringstream& output,
                                             const ir::Program& program,
                                             std::string_view name_space) {
  absl::flat_hash_map<DeclarationId, const ir::Declaration*> declarations;
  for (const auto& declaration : program.declarations) {
    declarations.emplace(declaration.identity, &declaration);
  }
  for (const auto& function : program.functions | std::views::filter(&ir::Function::is_exported)) {
    if (!function.signature) {
      std::unreachable();
    }
    const auto& signature = *function.signature;
    auto result = signature.result
                      ? HostLogicalType(program.layouts,
                                        program.layouts.types.at(*signature.result), name_space)
                      : std::expected<std::string, std::string>("void");
    if (!result) {
      return std::unexpected(function.name + ".result: " + result.error());
    }
    auto type = *result + '(';
    for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
      const auto& parameter = signature.parameters[index];
      auto argument = program.layouts.types.at(parameter.type);
      if (parameter.rest) {
        return std::unexpected(function.name + '.' + parameter.name +
                               ": entry requires a fixed positional signature");
      }
      if (parameter.optional) {
        argument.nullable = true;
      }
      const auto logical = HostLogicalType(program.layouts, argument, name_space);
      if (!logical) {
        return std::unexpected(function.name + '.' + parameter.name + ": " + logical.error());
      }
      type += (index == 0 ? "" : ", ") + *logical;
    }
    type += ')';
    std::string_view module = program.source_paths.at(function.location.source_id);
    std::string_view name = function.name;
    const auto declaration = declarations.find(function.identity.declaration);
    if (declaration != declarations.end()) {
      name = declaration->second->name;
      module = declarations.at(declaration->second->owner)->name;
    }
    const auto identifier = HostIdentifier(function.name);
    output << "using Entry_" << identifier << " = vela::Entry<" << type << ">;\n"
           << "inline vela::Result<Entry_" << identifier << "> BindEntry_" << identifier
           << "(vela::runtime::NativeInstance& instance) { return Entry_" << identifier
           << "::Bind(instance, " << Literal(function.name) << "); }\n"
           << "inline vela::Result<Entry_" << identifier << "> BindEntry_" << identifier
           << "(vela::runtime::NativeDispatch& dispatch) { return Entry_" << identifier
           << "::Bind(dispatch, " << Literal(module) << ", " << Literal(name) << "); }\n";
  }
  return {};
}

std::expected<std::string, std::string> CollectionLogicalType(const layout::Program& program,
                                                              layout::Type type,
                                                              std::string_view name_space) {
  const auto& collection = program.collections.at(type.index);
  auto key = HostLogicalType(program, collection.key, name_space);
  if (!key) {
    return std::unexpected(key.error());
  }
  if (!collection.value) {
    return "vela::Set<" + *key + '>';
  }
  auto value = HostLogicalType(program, *collection.value, name_space);
  return value ? std::expected<std::string, std::string>("vela::Map<" + *key + ", " + *value + '>')
               : std::unexpected(value.error());
}
std::expected<std::string, std::string> SignatureLogicalType(const layout::Program& program,
                                                             layout::Type type,
                                                             std::string_view name_space) {
  using Kind = layout::Type::Kind;
  const auto& signature = program.signatures.at(type.index);
  auto result = signature.result ? HostLogicalType(program, *signature.result, name_space)
                                 : std::expected<std::string, std::string>("void");
  if (!result) {
    return std::unexpected(result.error());
  }
  if (type.kind != Kind::Callable) {
    return std::string(type.kind == Kind::Future ? "vela::Future<" : "vela::Completer<") + *result +
           '>';
  }
  *result = "vela::Function<" + *result + '(';
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    const auto& parameter = signature.parameters[index];
    if (parameter.rest) {
      return std::unexpected("function values require a fixed positional signature: " +
                             signature.name);
    }
    const auto argument = HostLogicalType(program, parameter.type, name_space);
    if (!argument) {
      return std::unexpected(argument.error());
    }
    *result += (index == 0 ? "" : ", ") + *argument;
  }
  return *result + ")>";
}

void EmitTypeTags(std::ostringstream& output, const ir::Program& program) {
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    const auto& type = program.classes.at(index);
    const auto& name = program.layouts.classes.at(index).name;
    if (IsSourceClass(type) && IsExported(program.layouts, name)) {
      output << "struct Type_" << HostIdentifier(name) << "Tag final {};\n";
    }
  }
  for (const auto& name : program.layouts.interfaces) {
    if (IsExported(program.layouts, name)) {
      output << "struct Type_" << HostIdentifier(name) << "Tag final {};\n";
    }
  }
}

std::expected<void, std::string> EmitNominalTraits(std::ostringstream& output,
                                                   const ir::Program& program,
                                                   std::string_view name_space,
                                                   std::string_view name,
                                                   layout::Type::Kind kind,
                                                   std::string_view native_key) {
  const auto& nominal = program.layouts.nominals.at(std::string(name));
  std::string_view kind_name = native_key.empty() ? "Class" : "Host";
  if (kind == layout::Type::Kind::Enum) {
    kind_name = "Enum";
  } else if (kind == layout::Type::Kind::Interface) {
    kind_name = "Interface";
  }
  output << "template <> struct ScriptTypeTraits<" << Tag(name, name_space)
         << "> final { static constexpr auto kKind = ScriptTypeKind::" << kind_name
         << "; static constexpr std::string_view kModule = " << Literal(nominal.module)
         << "; static constexpr std::string_view kName = " << Literal(nominal.name)
         << "; using Arguments = std::tuple<";
  for (std::size_t index = 0; index < nominal.arguments.size(); ++index) {
    const auto argument = HostLogicalType(program.layouts, nominal.arguments[index], name_space);
    if (!argument) {
      return std::unexpected(nominal.name + ".argument[" + std::to_string(index) +
                             "]: " + argument.error());
    }
    output << (index == 0 ? "" : ", ") << *argument;
  }
  output << ">;";
  if (!native_key.empty()) {
    output << " static constexpr std::string_view kNativeKey = " << Literal(native_key) << ';';
  }
  output << " };\n";
  return {};
}

std::expected<void, std::string> EmitTypeTraits(std::ostringstream& output,
                                                const ir::Program& program,
                                                std::string_view name_space) {
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    const auto& type = program.classes.at(index);
    const auto& name = program.layouts.classes.at(index).name;
    if (IsSourceClass(type) && IsExported(program.layouts, name)) {
      const auto kind = type.kind == ir::Class::Kind::Enum ? layout::Type::Kind::Enum
                                                           : layout::Type::Kind::Instance;
      const auto emitted =
          EmitNominalTraits(output, program, name_space, name, kind, type.native_key);
      if (!emitted) {
        return std::unexpected(emitted.error());
      }
    }
  }
  for (const auto& name : program.layouts.interfaces) {
    if (IsExported(program.layouts, name)) {
      const auto emitted =
          EmitNominalTraits(output, program, name_space, name, layout::Type::Kind::Interface, {});
      if (!emitted) {
        return std::unexpected(emitted.error());
      }
    }
  }
  return {};
}

std::expected<void, std::string> EmitAliases(std::ostringstream& output,
                                             const ir::Program& program,
                                             std::string_view name_space) {
  absl::flat_hash_map<std::pair<layout::Type::Kind, std::uint32_t>,
                      std::vector<const MemberDefinition*>>
      members_by_receiver;
  for (const auto& member : program.members) {
    const auto type = program.layouts.types.at(member.receiver_type);
    if (!type.nullable) {
      members_by_receiver[{type.kind, type.index}].push_back(&member);
    }
  }
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    const auto& type = program.classes[index];
    if (!IsSourceClass(type) ||
        !IsExported(program.layouts, program.layouts.classes.at(index).name)) {
      continue;
    }
    const layout::Type receiver{
        .kind = type.kind == ir::Class::Kind::Enum ? layout::Type::Kind::Enum
                                                   : layout::Type::Kind::Instance,
        .index = static_cast<std::uint32_t>(index),
    };
    const auto logical = HostLogicalType(program.layouts, receiver, name_space);
    if (!logical) {
      return std::unexpected(logical.error());
    }
    output << "using Type_" << HostIdentifier(type.name) << " = " << *logical << ";\n";
    const auto members = EmitMembers(output, program, name_space, receiver, type.name,
                                     members_by_receiver[{receiver.kind, receiver.index}]);
    if (!members) {
      return std::unexpected(members.error());
    }
  }
  for (std::size_t index = 0; index < program.layouts.interfaces.size(); ++index) {
    const auto& name = program.layouts.interfaces[index];
    if (!IsExported(program.layouts, name)) {
      continue;
    }
    const layout::Type receiver{
        .kind = layout::Type::Kind::Interface,
        .index = static_cast<std::uint32_t>(index),
    };
    output << "using Type_" << HostIdentifier(name) << " = vela::Object<" << Tag(name, name_space)
           << ">;\n";
    const auto members = EmitMembers(output, program, name_space, receiver, name,
                                     members_by_receiver[{receiver.kind, receiver.index}]);
    if (!members) {
      return std::unexpected(members.error());
    }
  }
  return {};
}

}  // namespace

std::string HostIdentifier(std::string_view name) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string result;
  for (const auto character : name) {
    const auto byte = static_cast<std::uint8_t>(character);
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9')) {
      result += character;
    } else {
      result += '_';
      result += kHex[byte >> 4U];
      result += kHex[byte & (kHex.size() - 1U)];
    }
  }
  return result;
}

std::expected<std::string, std::string> HostLogicalType(const layout::Program& program,
                                                        layout::Type type,
                                                        std::string_view name_space) {
  using Kind = layout::Type::Kind;
  if (type.nullable && type.kind != Kind::Dynamic) {
    type.nullable = false;
    const auto required = HostLogicalType(program, type, name_space);
    return required ? std::expected<std::string, std::string>("vela::Nullable<" + *required + '>')
                    : std::unexpected(required.error());
  }
  switch (type.kind) {
    case Kind::Dynamic:
      return "vela::Value";
    case Kind::Boolean:
      return "bool";
    case Kind::Integer:
      return "std::int64_t";
    case Kind::Double:
      return "double";
    case Kind::String:
      return "vela::String";
    case Kind::MutableString:
      return "vela::MutableString";
    case Kind::ByteArray:
      return "vela::ByteArray";
    case Kind::ByteData:
      return "vela::ByteData";
    case Kind::RegExp:
      return "vela::RegExp";
    case Kind::RegExpMatch:
      return "vela::RegExpMatch";
    case Kind::Timer:
      return "vela::Timer";
    case Kind::CancellationToken:
      return "vela::CancellationToken";
    case Kind::Instance:
    case Kind::Enum: {
      const auto& nominal = program.classes.at(type.index);
      if (!IsExported(program, nominal.name)) {
        return std::unexpected("public signature references a declaration that is not exported: " +
                               nominal.name);
      }
      std::string_view wrapper = "Object";
      if (type.kind == Kind::Enum) {
        wrapper = "Enum";
      } else if (nominal.kind == layout::Class::Kind::Host) {
        wrapper = "Host";
      }
      return "vela::" + std::string(wrapper) + '<' + Tag(nominal.name, name_space) + '>';
    }
    case Kind::Interface: {
      const auto& name = program.interfaces.at(type.index);
      if (!IsExported(program, name)) {
        return std::unexpected("public signature references an interface that is not exported: " +
                               name);
      }
      return "vela::Object<" + Tag(name, name_space) + '>';
    }
    case Kind::Array: {
      const auto element =
          HostLogicalType(program, program.arrays.at(type.index).element, name_space);
      return element ? std::expected<std::string, std::string>("vela::Array<" + *element + '>')
                     : std::unexpected(element.error());
    }
    case Kind::Collection:
      return CollectionLogicalType(program, type, name_space);
    case Kind::Callable:
    case Kind::Future:
    case Kind::Completer:
      return SignatureLogicalType(program, type, name_space);
  }
  std::unreachable();
}

std::expected<std::string, std::string> EmitHostDeclarations(const ir::Program& program,
                                                             std::string_view name_space) {
  if (!IsHostNamespace(name_space)) {
    return std::unexpected("host namespace must contain non-reserved C++ identifiers");
  }
  std::ostringstream output;
  output << "#pragma once\n#include <tuple>\n#include <vela/host.h>\nnamespace " << name_space
         << " {\n";
  EmitTypeTags(output, program);
  output << "}\nnamespace vela {\n";
  const auto traits = EmitTypeTraits(output, program, name_space);
  if (!traits) {
    return std::unexpected(traits.error());
  }
  output << "}\nnamespace " << name_space << " {\n";
  const auto aliases = EmitAliases(output, program, name_space);
  if (!aliases) {
    return std::unexpected(aliases.error());
  }
  const auto entries = EmitEntries(output, program, name_space);
  if (!entries) {
    return std::unexpected(entries.error());
  }
  output << "}\n";
  return output.str();
}

}  // namespace vela::compiler::native
