#include "compiler/bindgen/emitter.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <llvm/Support/JSON.h>

#include "base/native_binding.h"
#include "base/path.h"
#include "compiler/bindgen/model.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/checker.h"
#include "compiler/tooling/format/formatter.h"

namespace vela::bindgen {
namespace {

std::string Quote(std::string_view text, char delimiter) {
  std::string result(1, delimiter);
  for (const char character : text) {
    if (character == '\\' || character == delimiter || (delimiter == '\'' && character == '$')) {
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

frontend::TypeName ScriptType(const Type& type, const TypeNames& names) {
  std::string name;
  switch (type.kind) {
    case TypeKind::Void:
      name = "void";
      break;
    case TypeKind::Boolean:
      name = "bool";
      break;
    case TypeKind::Integer:
      name = "int";
      break;
    case TypeKind::Double:
      name = "double";
      break;
    case TypeKind::String:
      name = "string";
      break;
    case TypeKind::Cancellation:
      name = "CancellationToken";
      break;
    case TypeKind::Host:
      name = names.at(type.key);
      break;
  }
  frontend::TypeName result{
      .name = std::move(name),
      .location = {},
      .kind = frontend::TypeName::Kind::Named,
      .arguments = {},
      .parameter = {},
  };
  if (type.nullable) {
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

frontend::Function ScriptMember(const Member& member, const TypeNames& names) {
  frontend::Function result;
  result.name = member.name;
  result.native_symbol = member.key;
  result.return_type = ScriptType(member.result, names);
  result.storage = member.kind == MemberKind::Static ? frontend::MemberStorage::Static
                                                     : frontend::MemberStorage::Instance;
  result.is_getter = member.kind == MemberKind::Getter;
  result.is_setter = member.kind == MemberKind::Setter;
  for (const auto& parameter : member.parameters) {
    result.parameters.push_back(
        {.type = ScriptType(parameter.type, names), .name = parameter.name, .location = {}});
  }
  return result;
}

frontend::Class ScriptClass(const Class& type, const TypeNames& names) {
  frontend::Class result;
  result.name = type.name;
  result.native_key = type.key;
  result.export_kind = frontend::ExportKind::Exported;
  result.constructor_kind = frontend::ConstructorKind::Unavailable;
  for (const auto& member : type.members) {
    auto method = ScriptMember(member, names);
    if (member.kind == MemberKind::Constructor) {
      method.storage = frontend::MemberStorage::Static;
      method.return_type = ScriptType(Type{}, names);
      result.constructor = std::make_unique<frontend::Function>(std::move(method));
      result.constructor_kind = frontend::ConstructorKind::Explicit;
    } else {
      result.methods.push_back(std::move(method));
    }
  }
  return result;
}

void RenderMember(std::ostream& output, const frontend::Function& method, bool constructor) {
  if (!constructor && method.storage == frontend::MemberStorage::Static) {
    output << "static ";
  }
  if (method.is_getter) {
    output << "get ";
  } else if (method.is_setter) {
    output << "set ";
  }
  output << (constructor ? "constructor" : method.name) << '(';
  for (std::size_t index = 0; index < method.parameters.size(); ++index) {
    const auto& parameter = method.parameters[index];
    if (index != 0) {
      output << ", ";
    }
    output << parameter.name << ": " << parameter.type.ToString();
  }
  output << ')';
  if (!constructor) {
    output << ": " << method.return_type.ToString();
  }
  output << ";\n";
}

std::expected<std::string, std::string> EmitScript(const Module& module,
                                                   const std::string& output_directory) {
  frontend::Program program;
  TypeNames names;
  std::set<std::string> identifiers;
  for (const auto& type : module.classes) {
    names.emplace(type.key, type.name);
    identifiers.insert(type.name);
  }
  for (std::size_t index = 0; index < module.dependencies.size(); ++index) {
    const auto& dependency = module.dependencies[index];
    auto alias = "nativeDependency" + std::to_string(index);
    while (!identifiers.insert(alias).second) {
      alias += '_';
    }
    const auto relative =
        Utf8Path(dependency.source).lexically_relative(Utf8Path(output_directory));
    program.imports.push_back({.path = PathText(relative), .location = {}, .alias = alias});
    for (const auto& type : dependency.classes) {
      names.emplace(type.key, alias + "." + type.name);
    }
  }
  for (const auto& type : module.classes) {
    program.classes.push_back(ScriptClass(type, names));
  }
  std::ostringstream output;
  for (const auto& import : program.imports) {
    output << "import " << Quote(import.path, '\'') << " as " << import.alias << ";\n";
  }
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    const auto& type = program.classes[index];
    const auto& description = module.classes[index];
    Documentation(output, description.documentation);
    output << "export external class " << type.name << " = " << Quote(type.native_key, '\'')
           << " {\n";
    if (type.constructor != nullptr) {
      RenderMember(output, *type.constructor, true);
    }
    std::size_t method_index = 0;
    for (const auto& member : description.members) {
      if (member.kind != MemberKind::Constructor) {
        Documentation(output, member.documentation);
        RenderMember(output, type.methods[method_index++], false);
      }
    }
    output << "}\n";
  }
  auto formatted = tooling::FormatSource(output.str());
  if (!formatted) {
    return std::unexpected(frontend::DiagnosticsText(formatted.error()));
  }
  const auto path = PathText(Utf8Path(output_directory) / (module.name + ".vela"));
  const frontend::SourceInput candidate{.path = path, .text = *formatted};
  const auto checked = frontend::CheckParsed(frontend::LoadProgram(path, {&candidate, 1}));
  if (!checked.Succeeded()) {
    return std::unexpected(frontend::DiagnosticsText(checked.diagnostics));
  }
  return std::move(*formatted);
}

std::string EmitHeader(const Module& module, const std::vector<std::string>& headers) {
  std::ostringstream output;
  output << "#pragma once\n#include <expected>\n#include <string_view>\n"
            "#include \"runtime/isolate/isolate.h\"\n"
            "#include \"runtime/native/native_host_type.h\"\n";
  for (const auto& header : headers) {
    output << "#include " << Quote(PathText(Utf8Path(header)), '"') << '\n';
  }
  for (const auto& dependency : module.dependencies) {
    output << "#include " << Quote(PathText(Utf8Path(dependency.header)), '"') << '\n';
  }
  output << "namespace vela::runtime {\n";
  for (const auto& type : module.classes) {
    output << "template <> struct NativeClassTraits<::" << type.cpp << "> final {\n"
           << "static constexpr std::string_view kKey = " << Quote(type.key, '"') << ";\n};\n";
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

std::string EmitCpp(const Module& module) {
  std::ostringstream output;
  output << "#include " << Quote(module.name + "_bindings.h", '"')
         << "\n"
            "#include <utility>\n#include <vector>\n"
            "#include \"runtime/native/native_class.h\"\n"
            "#include \"runtime/native/native_context.h\"\n"
            "namespace {\n";
  std::size_t index = 0;
  for (const auto& type : module.classes) {
    for (const auto& member : type.members) {
      EmitThunk(output, type, member, "NativeThunk" + std::to_string(index++));
    }
  }
  output << "}\nnamespace " << RegistrationNamespace(module.name) << " {\n"
         << "std::expected<void, runtime::NativeRegistrationError> Register(runtime::Isolate& "
            "isolate) {\n"
            "std::vector<runtime::NativeClass> classes;\n";
  index = 0;
  for (const auto& type : module.classes) {
    output << "{\nstd::vector<runtime::NativeClassMember> members;\n";
    for (const auto& member : type.members) {
      const auto kind = static_cast<unsigned>(member.kind);
      output << "members.push_back({.kind = static_cast<NativeMemberKind>(" << kind
             << "), .name = " << Quote(member.name, '"') << ", .parameter_names = {";
      bool comma = false;
      for (const auto& parameter : member.parameters) {
        output << (comma ? ", " : "") << Quote(parameter.name, '"');
        comma = true;
      }
      const auto target = "NativeThunk" + std::to_string(index++);
      output << "}, .function = runtime::NativeFunction::Bind<"
             << (member.kind == MemberKind::Static ? target + "Target" : "&" + target) << ">("
             << Quote(member.key, '"') << ")});\n";
    }
    output << "auto type = runtime::NativeClass::Define<::" << type.cpp
           << ">(std::move(members));\ntype.contract = " << Quote(PublicContract(type), '"')
           << ";\nclasses.push_back(std::move(type));\n}\n";
  }
  output << "return isolate.RegisterNativeModule(std::move(classes));\n}\n}\n";
  return output.str();
}

}  // namespace

std::expected<llvm::json::Object, std::string> Emit(const Module& module,
                                                    const std::vector<std::string>& headers,
                                                    const std::string& output_directory) {
  auto script = EmitScript(module, output_directory);
  if (!script) {
    return std::unexpected(script.error());
  }
  llvm::json::Object products;
  products[module.name + ".vela"] = std::move(*script);
  products[module.name + "_bindings.h"] = EmitHeader(module, headers);
  products[module.name + "_bindings.cc"] = EmitCpp(module);
  return products;
}

}  // namespace vela::bindgen
