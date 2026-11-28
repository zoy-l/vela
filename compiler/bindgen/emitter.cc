#include "compiler/bindgen/emitter.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <llvm/Support/JSON.h>

#include "base/fingerprint.h"
#include "base/layout_description.h"
#include "base/native_binding.h"
#include "base/path.h"
#include "base/type_kind.h"
#include "compiler/bindgen/model.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/lexer.h"
#include "compiler/frontend/typing/checker.h"
#include "compiler/host_type_contract.h"
#include "compiler/tooling/format/formatter.h"
#include "vela/types.h"

namespace vela::bindgen {
namespace {

std::string Quote(std::string_view text, char delimiter) {
  std::string result(1, delimiter);
  for (const char character : text) {
    if (character == '\\' || character == delimiter) {
      result += '\\';
    }
    result += character;
  }
  return result + delimiter;
}

void Documentation(std::ostream& output, const std::string& text) {
  std::istringstream input(text);
  for (std::string line; std::getline(input, line);) {
    output << "/// " << line << '\n';
  }
}

using TypeNames = std::map<std::string, std::string>;

frontend::TypeName ScriptType(const layout::Program& program,
                              std::optional<layout::Type> type,
                              const TypeNames& names);

frontend::TypeName ScriptNominal(const layout::Program& program,
                                 const std::string& key,
                                 const TypeNames& names) {
  const auto& nominal = program.nominals.at(key);
  frontend::TypeName result{
      .name = names.at(key),
      .location = {},
      .kind = nominal.arguments.empty() ? frontend::TypeName::Kind::Named
                                        : frontend::TypeName::Kind::Generic,
      .arguments = {},
      .parameter = {},
  };
  for (const auto& argument : nominal.arguments) {
    result.arguments.push_back(ScriptType(program, argument, names));
  }
  return result;
}

frontend::TypeName ScriptType(const layout::Program& program,
                              std::optional<layout::Type> type,
                              const TypeNames& names) {
  frontend::TypeName result{
      .name = {},
      .location = {},
      .kind = frontend::TypeName::Kind::Named,
      .arguments = {},
      .parameter = {},
  };
  if (!type) {
    result.name = BuiltinTypeName(LanguageTypeKind::Void);
    return result;
  }
  using Kind = layout::Type::Kind;
  switch (type->kind) {
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
      result.name = layout::ScalarTypeName(type->kind);
      break;
    case Kind::Class:
    case Kind::Enum: {
      const auto& nominal = program.classes.at(type->index);
      if (nominal.kind == layout::Class::Kind::Host) {
        result.name = names.at(nominal.native_key);
      } else {
        result = ScriptNominal(program, nominal.name, names);
      }
      break;
    }
    case Kind::Interface:
      result = ScriptNominal(program, program.interfaces.at(type->index), names);
      break;
    case Kind::Array:
      result.kind = frontend::TypeName::Kind::Array;
      result.arguments.push_back(
          ScriptType(program, program.arrays.at(type->index).element, names));
      break;
    case Kind::Set:
    case Kind::Map: {
      const auto& collection = program.collections.at(type->index);
      result.kind = frontend::TypeName::Kind::Generic;
      result.name = BuiltinTypeName(type->kind);
      result.arguments.push_back(ScriptType(program, collection.key, names));
      if (collection.value) {
        result.arguments.push_back(ScriptType(program, collection.value, names));
      }
      break;
    }
    case Kind::Function:
    case Kind::Future:
    case Kind::Completer: {
      const auto& signature = program.signatures.at(type->index);
      if (type->kind == Kind::Function) {
        result.kind = frontend::TypeName::Kind::Function;
        for (const auto& parameter : signature.parameters) {
          result.arguments.push_back(ScriptType(program, parameter.type, names));
        }
      } else {
        result.kind = frontend::TypeName::Kind::Generic;
        result.name = BuiltinTypeName(type->kind);
      }
      result.arguments.push_back(ScriptType(program, signature.result, names));
      break;
    }
    case Kind::Invalid:
    case Kind::Void:
    case Kind::Parameter:
      std::unreachable();
  }
  if (type->nullable) {
    result = {
        .name = {},
        .location = {},
        .kind = frontend::TypeName::Kind::Nullable,
        .arguments = {std::move(result)},
        .parameter = {},
    };
  }
  return result;
}

frontend::TypeName ScriptType(const Type& type, const TypeNames& names) {
  return ScriptType(type.layouts, type.value, names);
}

void RenderMember(std::ostream& output, const Member& member, const TypeNames& names) {
  const auto constructor = member.kind == MemberKind::Constructor;
  if (member.kind == MemberKind::Static) {
    output << "static ";
  }
  if (member.kind == MemberKind::Getter) {
    output << "get ";
  } else if (member.kind == MemberKind::Setter) {
    output << "set ";
  }
  output << (constructor ? "constructor" : member.name) << '(';
  for (std::size_t index = 0; index < member.parameters.size(); ++index) {
    const auto& parameter = member.parameters[index];
    if (index != 0) {
      output << ", ";
    }
    output << parameter.name << ": " << ScriptType(parameter.type, names).ToString();
  }
  output << ')';
  if (!constructor) {
    output << ": " << ScriptType(member.result, names).ToString();
  }
  output << ";\n";
}

std::string DependencyAlias(std::string_view prefix,
                            std::size_t index,
                            std::set<std::string>& identifiers) {
  auto alias = std::string(prefix) + std::to_string(index);
  while (!identifiers.insert(alias).second) {
    alias += '_';
  }
  return alias;
}

void ImportNativeDependencies(const Module& module,
                              const std::string& output_directory,
                              std::vector<frontend::Program::Import>& imports,
                              TypeNames& names,
                              std::set<std::string>& identifiers) {
  for (const auto& type : module.classes) {
    names.emplace(type.key, type.name);
    identifiers.insert(type.name);
  }
  for (std::size_t index = 0; index < module.dependencies.size(); ++index) {
    const auto& dependency = module.dependencies[index];
    const auto alias = DependencyAlias("nativeDependency", index, identifiers);
    const auto relative =
        Utf8Path(dependency.source).lexically_relative(Utf8Path(output_directory));
    imports.push_back({
        .path = PathText(relative),
        .location = {},
        .alias = alias,
        .diagnostic_location = {},
    });
    for (const auto& type : dependency.classes) {
      names.emplace(type.key, alias + "." + type.name);
    }
  }
}

std::expected<void, std::string> CollectScriptType(
    TypeNames& names,
    std::set<std::string>& identifiers,
    std::map<std::string, std::string>& script_imports,
    const Type& type,
    const std::string& member) {
  for (const auto& nominal : type.layouts.classes) {
    if (nominal.kind == layout::Class::Kind::Host && !names.contains(nominal.native_key)) {
      return std::unexpected(member + ": host type " + nominal.native_key +
                             " requires its binding dependency manifest");
    }
  }
  for (const auto& [key, nominal] : type.layouts.nominals) {
    if (names.contains(key)) {
      continue;
    }
    const auto [entry, inserted] = script_imports.try_emplace(nominal.module);
    if (inserted) {
      entry->second = DependencyAlias("scriptDependency", script_imports.size(), identifiers);
    }
    names.emplace(key, entry->second + '.' + nominal.name);
  }
  return {};
}

std::expected<void, std::string> ImportScriptDependencies(
    const Module& module,
    const std::string& output_directory,
    std::vector<frontend::Program::Import>& imports,
    TypeNames& names,
    std::set<std::string>& identifiers) {
  std::map<std::string, std::string> script_imports;
  for (const auto& type : module.classes) {
    for (const auto& member : type.members) {
      const auto name = type.name + '.' + member.name;
      if (const auto result = CollectScriptType(names, identifiers, script_imports, member.result,
                                                name + ".result");
          !result) {
        return std::unexpected(result.error());
      }
      for (const auto& parameter : member.parameters) {
        if (const auto result = CollectScriptType(names, identifiers, script_imports,
                                                  parameter.type, name + '.' + parameter.name);
            !result) {
          return std::unexpected(result.error());
        }
      }
    }
  }
  for (const auto& [source, alias] : script_imports) {
    const auto relative = Utf8Path(source).lexically_relative(Utf8Path(output_directory));
    imports.push_back({
        .path = relative.empty() ? source : PathText(relative),
        .location = {},
        .alias = alias,
        .diagnostic_location = {},
    });
  }
  return {};
}

std::string RenderScript(const Module& module,
                         std::span<const frontend::Program::Import> imports,
                         const TypeNames& names) {
  std::ostringstream output;
  for (const auto& import : imports) {
    output << "import " << frontend::QuoteStringLiteral(import.path) << " as " << import.alias
           << ";\n";
  }
  for (const auto& type : module.classes) {
    Documentation(output, type.documentation);
    if (!type.enumerators.empty()) {
      output << "export enum " << type.name << " {";
      for (const auto& [name, unused] : type.enumerators) {
        output << name << ',';
      }
      output << "}\n";
      continue;
    }
    output << "export external class " << type.name << " = "
           << frontend::QuoteStringLiteral(type.key) << " {\n";
    const auto constructor =
        std::ranges::find(type.members, MemberKind::Constructor, &Member::kind);
    if (constructor != type.members.end()) {
      RenderMember(output, *constructor, names);
    }
    for (const auto& member : type.members) {
      if (member.kind != MemberKind::Constructor) {
        Documentation(output, member.documentation);
        RenderMember(output, member, names);
      }
    }
    output << "}\n";
  }
  return output.str();
}

std::expected<std::string, std::string> EmitScript(Module& module,
                                                   const std::string& output_directory) {
  std::vector<frontend::Program::Import> imports;
  TypeNames names;
  std::set<std::string> identifiers;
  ImportNativeDependencies(module, output_directory, imports, names, identifiers);
  for (const auto& type : module.classes) {
    if (!type.enumerators.empty()) {
      layout::Program layouts;
      layout::Nominal nominal{
          .module = PathText(Utf8Path(output_directory) / (module.name + ".vela")),
          .name = type.name,
          .arguments = {},
          .exported = true,
      };
      names.emplace(layout::NominalKey(layouts, nominal), type.name);
    }
  }
  const auto imported =
      ImportScriptDependencies(module, output_directory, imports, names, identifiers);
  if (!imported) {
    return std::unexpected(imported.error());
  }
  auto formatted = tooling::FormatSource(RenderScript(module, imports, names));
  if (!formatted) {
    return std::unexpected(frontend::DiagnosticsText(formatted.error()));
  }
  const auto path = PathText(Utf8Path(output_directory) / (module.name + ".vela"));
  const frontend::SourceInput candidate{.path = path, .text = *formatted};
  auto loaded = frontend::LoadProgram(path, {&candidate, 1});
  if (frontend::HasErrors(loaded.diagnostics)) {
    return std::unexpected(frontend::DiagnosticsText(loaded.diagnostics));
  }
  const auto checked = frontend::CheckProgram(loaded.program);
  if (!checked.Succeeded()) {
    return std::unexpected(frontend::DiagnosticsText(checked.diagnostics));
  }
  for (const auto& source : loaded.program.sources) {
    if (source.path == path || source.path.starts_with("builtin:") ||
        source.path.starts_with("sdk:")) {
      continue;
    }
    module.input_fingerprints.emplace(source.path, ContentFingerprint(source.text).Hex());
  }
  return std::move(*formatted);
}

std::string EmitHeader(const Module& module, const std::set<std::string>& headers) {
  std::ostringstream output;
  output << "#pragma once\n#include <expected>\n#include <string_view>\n"
            "#include \"runtime/isolate/isolate.h\"\n"
            "#include \"runtime/native/native_host_type.h\"\n"
            "#include <vela/native_values.h>\n";
  for (const auto& header : headers) {
    output << "#include " << Quote(PathText(Utf8Path(header)), '"') << '\n';
  }
  for (const auto& dependency : module.dependencies) {
    output << "#include " << Quote(PathText(Utf8Path(dependency.header)), '"') << '\n';
  }
  output << "namespace vela::runtime {\n";
  for (const auto& type : module.classes) {
    if (!type.enumerators.empty()) {
      continue;
    }
    output << "template <> struct NativeClassTraits<::" << type.cpp << "> final {\n"
           << "static constexpr std::string_view kKey = " << Quote(type.key, '"') << ";\n};\n";
  }
  output << "}\nnamespace vela {\n";
  for (const auto& type : module.classes) {
    if (type.enumerators.empty()) {
      continue;
    }
    output << "template <> struct " << compiler::host_types::kScriptTraitsName << "<::" << type.cpp
           << "> {\n"
           << "static constexpr auto " << compiler::host_types::kScriptKindMember << " = "
           << compiler::host_types::kScriptKindTypeName
           << "::" << compiler::host_types::ScriptKindName(ScriptTypeKind::Enum) << ";\n"
           << "static constexpr std::string_view " << compiler::host_types::kScriptModuleMember
           << " = " << Quote(module.source, '"') << ";\n"
           << "static constexpr std::string_view " << compiler::host_types::kScriptNameMember
           << " = " << Quote(type.name, '"') << ";\n"
           << "using " << compiler::host_types::kScriptArgumentsMember << " = std::tuple<>; };\n";
  }
  output << "}\nnamespace vela::bindings {\n";
  for (const auto& type : module.classes) {
    if (type.enumerators.empty()) {
      continue;
    }
    output << "template <> struct EnumValues<::" << type.cpp << "> {\n"
           << "static constexpr std::array<std::pair<std::string_view, ::" << type.cpp << ">, "
           << type.enumerators.size() << "> values{{";
    for (const auto& [name, value] : type.enumerators) {
      output << '{' << Quote(name, '"') << ", ::" << value << "},";
    }
    output << "}}; };\n";
  }
  output << "}\nnamespace " << RegistrationNamespace(module.name) << " {\n"
         << "std::expected<void, runtime::NativeRegistrationError> Register(runtime::Isolate& "
            "isolate);\n"
         << "}\n";
  return output.str();
}

void EmitThunk(std::ostream& output,
               const Class& type,
               const Member& member,
               const std::string& name) {
  if (member.implementation) {
    auto implementation = *member.implementation;
    implementation.replace(implementation.find("$name"), std::string_view("$name").size(), name);
    output << implementation;
    return;
  }
  const bool constructor = member.kind == MemberKind::Constructor;
  const bool receiver = HasNativeReceiver(member.kind);
  if (!constructor) {
    output << "using " << name << "Pointer = " << member.cpp_pointer_type << ";\n"
           << "constexpr auto " << name << "Target = static_cast<" << name
           << "Pointer>(&::" << member.cpp_name << ");\n";
  }
  if (member.kind == MemberKind::Static) {
    return;
  }
  output << "auto " << name << '(';
  bool comma = false;
  if (constructor || member.context) {
    output << "::vela::runtime::NativeContext& context";
    comma = true;
  }
  if (receiver) {
    output << (comma ? ", " : "") << (member.is_const ? "const " : "") << "::" << type.cpp
           << "& receiver";
    comma = true;
  }
  for (std::size_t index = 0; index < member.parameters.size(); ++index) {
    output << (comma ? ", " : "") << member.parameters[index].type.cpp << " arg" << index;
    comma = true;
  }
  output << ") noexcept {\nreturn ";
  if (constructor) {
    output << "context.Construct<::" << type.cpp << ">(";
  } else if (receiver) {
    output << "(receiver.*" << name << "Target)(";
  } else {
    output << name << "Target(";
  }
  comma = false;
  if (member.context) {
    output << "context";
    comma = true;
  }
  for (std::size_t index = 0; index < member.parameters.size(); ++index) {
    output << (comma ? ", " : "") << "arg" << index;
    comma = true;
  }
  output << ");\n}\n";
}

std::string ThunkName(std::size_t type, std::size_t member) {
  return "NativeThunk" + std::to_string(type) + "_" + std::to_string(member);
}

void EmitClassFactory(std::ostream& output, const Class& type, std::size_t index) {
  output << "runtime::NativeClass MakeNativeClass" << index << "() {\n"
         << "std::vector<runtime::NativeClassMember> members;\n"
         << "members.reserve(" << type.members.size() << ");\n";
  for (std::size_t member_index = 0; member_index < type.members.size(); ++member_index) {
    const auto& member = type.members[member_index];
    const auto kind = static_cast<unsigned>(member.kind);
    output << "members.push_back({.kind = static_cast<NativeMemberKind>(" << kind
           << "), .name = " << Quote(member.name, '"') << ", .parameter_names = {";
    bool comma = false;
    for (const auto& parameter : member.parameters) {
      output << (comma ? ", " : "") << Quote(parameter.name, '"');
      comma = true;
    }
    const auto target = ThunkName(index, member_index);
    output << "}, .function = runtime::NativeFunction::Bind<"
           << (member.kind == MemberKind::Static && !member.implementation ? target + "Target"
                                                                           : "&" + target)
           << ">(" << Quote(member.key, '"') << ")});\n";
  }
  output << "auto type = runtime::NativeClass::Define<::" << type.cpp
         << ">(std::move(members));\ntype.contract = " << Quote(PublicContract(type), '"')
         << ";\nreturn type;\n}\n";
}

void EmitRegistration(std::ostream& output, std::size_t class_count) {
  output << "namespace detail {\n";
  for (std::size_t index = 0; index < class_count; ++index) {
    output << "runtime::NativeClass MakeNativeClass" << index << "();\n";
  }
  output << "}\nstd::expected<void, runtime::NativeRegistrationError> Register(runtime::Isolate& "
            "isolate) {\n"
            "std::vector<runtime::NativeClass> classes;\n"
         << "classes.reserve(" << class_count << ");\n";
  for (std::size_t index = 0; index < class_count; ++index) {
    output << "classes.push_back(detail::MakeNativeClass" << index << "());\n";
  }
  output << "return isolate.RegisterNativeModule(std::move(classes));\n}\n";
}

void EmitCpp(const Module& module, std::size_t shard_count, llvm::json::Object& products) {
  std::vector<const Class*> classes;
  std::vector<std::size_t> order;
  for (const auto& type : module.classes) {
    if (type.enumerators.empty()) {
      order.push_back(classes.size());
      classes.push_back(&type);
    }
  }
  // 先分配成员最多的类型，登记仍按原有类型顺序执行。
  std::ranges::stable_sort(order, std::greater<>{},
                           [&](std::size_t index) { return classes[index]->members.size(); });
  struct Shard final {
    std::size_t weight = 0;
    std::vector<std::size_t> classes;
  };
  std::vector<Shard> shards(shard_count);
  for (const auto index : order) {
    auto& shard = *std::ranges::min_element(shards, {}, &Shard::weight);
    shard.classes.push_back(index);
    shard.weight += 1 + classes[index]->members.size();
  }
  const auto registration_namespace = RegistrationNamespace(module.name);
  const auto basename = module.name + "_bindings";
  for (std::size_t shard_index = 0; shard_index < shard_count; ++shard_index) {
    const auto& shard = shards[shard_index];
    std::ostringstream output;
    output << "#include " << Quote(basename + ".h", '"')
           << "\n#include <utility>\n#include <vector>\n"
              "#include \"runtime/native/native_class.h\"\n"
              "#include \"runtime/native/native_context.h\"\n"
              "namespace {\n";
    for (const auto index : shard.classes) {
      const auto& type = *classes[index];
      for (std::size_t member = 0; member < type.members.size(); ++member) {
        EmitThunk(output, type, type.members[member], ThunkName(index, member));
      }
    }
    output << "}\nnamespace " << registration_namespace << " {\nnamespace detail {\n";
    for (const auto index : shard.classes) {
      EmitClassFactory(output, *classes[index], index);
    }
    output << "}\n";
    if (shard_index == 0) {
      EmitRegistration(output, classes.size());
    }
    output << "}\n";
    const auto suffix = shard_index == 0 ? "" : "_" + std::to_string(shard_index);
    products[basename + suffix + ".cc"] = output.str();
  }
}

}  // namespace

std::expected<llvm::json::Object, std::string> Emit(Module& module,
                                                    const std::set<std::string>& headers,
                                                    const std::string& output_directory,
                                                    std::size_t cpp_shards) {
  auto script = EmitScript(module, output_directory);
  if (!script) {
    return std::unexpected(script.error());
  }
  llvm::json::Object products;
  products[module.name + ".vela"] = std::move(*script);
  products[module.name + "_bindings.h"] = EmitHeader(module, headers);
  EmitCpp(module, cpp_shards, products);
  return products;
}

}  // namespace vela::bindgen
