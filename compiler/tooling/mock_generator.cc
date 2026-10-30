#include "compiler/tooling/mock_generator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/fingerprint.h"
#include "base/path.h"
#include "base/source_location.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/sdk_modules.h"
#include "compiler/tooling/formatter.h"

namespace vela::tooling {
namespace {

constexpr std::string_view kGeneratorVersion = "1";
constexpr std::string_view kDescriptorVersion = "1";
enum class AdapterKind : std::uint8_t { Implementation, Pattern };

enum class AliasMismatch : std::uint8_t { Type };
using AliasArguments = absl::flat_hash_map<std::string, std::optional<frontend::TypeName>>;

struct GenericAlias final {
  std::string name;
  frontend::TypeName target;
  std::vector<std::string> parameters;
};

std::expected<void, AliasMismatch> BindAliasType(const frontend::TypeName& pattern,
                                                 const frontend::TypeName& actual,
                                                 AliasArguments& arguments) {
  const auto parameter = arguments.find(pattern.name);
  if (parameter != arguments.end()) {
    auto& bound = parameter->second;
    if (bound && bound->ToString() != actual.ToString()) {
      return std::unexpected(AliasMismatch::Type);
    }
    bound = actual;
    return {};
  }
  if (pattern.kind != actual.kind || pattern.name != actual.name ||
      pattern.arguments.size() != actual.arguments.size()) {
    return std::unexpected(AliasMismatch::Type);
  }
  for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
    const auto bound =
        BindAliasType(pattern.arguments.at(index), actual.arguments.at(index), arguments);
    if (!bound) {
      return bound;
    }
  }
  return {};
}

std::string Quote(std::string_view value) {
  std::string result = "'";
  for (const char character : value) {
    switch (character) {
      case '\\':
        result += "\\\\";
        break;
      case '\'':
        result += "\\'";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '$':
        result += "\\$";
        break;
      default:
        result += character;
        break;
    }
  }
  return result + '\'';
}

std::string RelativeModule(std::string_view module, const std::filesystem::path& directory) {
  if (module.starts_with(frontend::kSdkSourcePrefix) || module.starts_with("builtin:")) {
    return std::string(module);
  }
  return PathText(Utf8Path(module).lexically_relative(directory));
}

class MockGenerator final {
 public:
  MockGenerator(const frontend::Program& program, const std::filesystem::path& output);
  std::expected<std::string, frontend::Diagnostics> Generate(std::span<const std::string> names);

 private:
  std::string AllocateName(std::string_view preferred);
  std::string Import(std::string_view module);
  void Error(SourceLocation location, std::string message);
  [[nodiscard]] frontend::TypeName Expand(const frontend::TypeName& type) const;
  frontend::TypeName NameType(const frontend::TypeName& type);
  void RegisterPublicAlias(const frontend::Program::TypeAlias& alias);
  std::optional<frontend::TypeName> NameGenericAlias(const frontend::TypeName& type);
  std::string PublicName(const std::string& name, SourceLocation location);
  void EmitInterface(const frontend::Interface& declaration);
  void EmitAdapter(const frontend::Interface& declaration,
                   std::span<const std::string> fields,
                   AdapterKind kind,
                   std::string_view name);
  void Parameters(const frontend::Function& method, AdapterKind kind);
  void Arguments(const frontend::Function& method);
  [[nodiscard]] std::string Fingerprint(std::span<const frontend::Interface* const> selected) const;

  const frontend::Program* program_;
  std::filesystem::path directory_;
  std::filesystem::path api_directory_;
  absl::flat_hash_set<std::string> identifiers_;
  absl::flat_hash_set<std::string> exported_;
  absl::flat_hash_map<std::string, const frontend::Program::TypeAlias*> aliases_;
  std::map<std::string, std::string> public_aliases_;
  std::map<std::pair<frontend::TypeName::Kind, std::string>, std::vector<GenericAlias>>
      generic_aliases_;
  absl::flat_hash_set<std::string> naming_aliases_;
  std::map<std::string, std::string> imports_;
  std::string sdk_;
  std::ostringstream body_;
  frontend::Diagnostics diagnostics_;
};

MockGenerator::MockGenerator(const frontend::Program& program, const std::filesystem::path& output)
    : program_(&program),
      directory_(output.parent_path()),
      api_directory_(Utf8Path(program.sources.front().path).parent_path()) {
  for (const auto& [name, origin] : program.origins) {
    identifiers_.insert(origin.name);
  }
  const auto collect = [this](const auto& declarations) {
    for (const auto& declaration : declarations) {
      if (declaration.export_kind == frontend::ExportKind::Exported) {
        exported_.insert(declaration.name);
      }
    }
  };
  collect(program.classes);
  collect(program.interfaces);
  collect(program.enums);
  collect(program.aliases);
  for (const auto& alias : program.aliases) {
    aliases_.emplace(alias.name, &alias);
  }
  // 公开透明别名可为私有实现类型提供合法名称；按规范声明顺序稳定选择。
  for (const auto& alias : program.aliases) {
    if (alias.export_kind == frontend::ExportKind::Exported) {
      RegisterPublicAlias(alias);
    }
  }
  for (const auto& declaration : program.interfaces) {
    for (const auto& method : declaration.methods) {
      identifiers_.insert(method.name);
      for (const auto& parameter : method.parameters) {
        identifiers_.insert(parameter.name);
      }
    }
  }
}

void MockGenerator::RegisterPublicAlias(const frontend::Program::TypeAlias& alias) {
  if (alias.type_parameters.empty()) {
    public_aliases_.try_emplace(Expand(alias.target).ToString(), alias.name);
    return;
  }
  frontend::TypeName application{
      .name = alias.name,
      .location = alias.location,
      .kind = frontend::TypeName::Kind::Generic,
      .arguments = {},
  };
  std::vector<std::string> parameters;
  parameters.reserve(alias.type_parameters.size());
  for (std::size_t index = 0; index < alias.type_parameters.size(); ++index) {
    const auto name = "$mockParameter" + std::to_string(index);
    parameters.push_back(name);
    application.arguments.push_back({.name = name, .location = alias.location, .arguments = {}});
  }
  auto target = Expand(application);
  // Identity<T> 一类别名仍需公开命名 T，不能为私有类型提供新名字。
  if (std::ranges::find(parameters, target.name) != parameters.end()) {
    return;
  }
  generic_aliases_[{target.kind, target.name}].push_back({
      .name = alias.name,
      .target = std::move(target),
      .parameters = std::move(parameters),
  });
}

std::optional<frontend::TypeName> MockGenerator::NameGenericAlias(const frontend::TypeName& type) {
  const auto candidates = generic_aliases_.find({type.kind, type.name});
  if (candidates == generic_aliases_.end()) {
    return std::nullopt;
  }
  for (const auto& alias : candidates->second) {
    if (naming_aliases_.contains(alias.name)) {
      continue;
    }
    AliasArguments arguments;
    for (const auto& parameter : alias.parameters) {
      arguments.emplace(parameter, std::nullopt);
    }
    if (!BindAliasType(alias.target, type, arguments)) {
      continue;
    }
    if (std::ranges::any_of(arguments, [](const auto& entry) { return !entry.second; })) {
      continue;
    }
    frontend::TypeName result{
        .name = PublicName(alias.name, type.location),
        .location = type.location,
        .kind = frontend::TypeName::Kind::Generic,
        .arguments = {},
    };
    naming_aliases_.insert(alias.name);
    for (const auto& parameter : alias.parameters) {
      result.arguments.push_back(NameType(*arguments.at(parameter)));
    }
    naming_aliases_.erase(alias.name);
    return result;
  }
  return std::nullopt;
}

std::string MockGenerator::AllocateName(std::string_view preferred) {
  std::string name(preferred);
  std::size_t suffix = 0;
  while (!identifiers_.insert(name).second) {
    name = std::string(preferred) + std::to_string(++suffix);
  }
  return name;
}

std::string MockGenerator::Import(std::string_view module) {
  const auto found = imports_.find(std::string(module));
  if (found != imports_.end()) {
    return found->second;
  }
  const auto name = AllocateName("_mockModule");
  imports_.emplace(module, name);
  return name;
}

void MockGenerator::Error(SourceLocation location, std::string message) {
  diagnostics_.push_back({
      .location = location,
      .message = std::move(message),
      .source_path = program_->sources.at(location.source_id).path,
  });
}

frontend::TypeName MockGenerator::Expand(const frontend::TypeName& type) const {
  auto result = type;
  for (auto& argument : result.arguments) {
    argument = Expand(argument);
  }
  const auto alias = aliases_.find(type.name);
  if (alias == aliases_.end()) {
    return result;
  }
  const auto& declaration = *alias->second;
  absl::flat_hash_map<std::string, frontend::TypeName> substitutions;
  for (std::size_t index = 0; index < declaration.type_parameters.size(); ++index) {
    substitutions.emplace(declaration.type_parameters.at(index), result.arguments.at(index));
  }
  const auto substitute = [&substitutions](auto&& self,
                                           frontend::TypeName input) -> frontend::TypeName {
    if (const auto found = substitutions.find(input.name); found != substitutions.end()) {
      return found->second;
    }
    for (auto& argument : input.arguments) {
      argument = self(self, std::move(argument));
    }
    return input;
  };
  return Expand(substitute(substitute, declaration.target));
}

std::string MockGenerator::PublicName(const std::string& name, SourceLocation location) {
  const auto origin = program_->origins.find(name);
  if (origin == program_->origins.end() || origin->second.module_uri.starts_with("builtin:")) {
    return name;
  }
  if (!exported_.contains(name)) {
    Error(location, "mock signature type '" + origin->second.name + "' has no public name");
    return name;
  }
  return Import(origin->second.module_uri) + '.' + origin->second.name;
}

frontend::TypeName MockGenerator::NameType(const frontend::TypeName& type) {
  const auto origin = program_->origins.find(type.name);
  if (origin != program_->origins.end() && exported_.contains(type.name)) {
    auto result = type;
    result.name = PublicName(type.name, type.location);
    for (auto& argument : result.arguments) {
      argument = NameType(argument);
    }
    return result;
  }
  const auto expanded = Expand(type);
  const auto alias = public_aliases_.find(expanded.ToString());
  if (alias != public_aliases_.end()) {
    return {
        .name = PublicName(alias->second, type.location),
        .location = type.location,
        .arguments = {},
    };
  }
  if (auto named = NameGenericAlias(expanded)) {
    return *named;
  }
  auto result = expanded;
  if (result.kind == frontend::TypeName::Kind::Named ||
      result.kind == frontend::TypeName::Kind::Generic) {
    result.name = PublicName(result.name, type.location);
  }
  for (auto& argument : result.arguments) {
    argument = NameType(argument);
  }
  return result;
}

void MockGenerator::Parameters(const frontend::Function& method, AdapterKind kind) {
  body_ << '(';
  for (std::size_t index = 0; index < method.parameters.size(); ++index) {
    const auto& parameter = method.parameters.at(index);
    if (index != 0) {
      body_ << ", ";
    }
    body_ << parameter.name << ": "
          << (kind == AdapterKind::Pattern ? "dynamic" : NameType(parameter.type).ToString());
  }
  body_ << ')';
}

void MockGenerator::Arguments(const frontend::Function& method) {
  body_ << '[';
  for (std::size_t index = 0; index < method.parameters.size(); ++index) {
    if (index != 0) {
      body_ << ", ";
    }
    body_ << method.parameters.at(index).name;
  }
  body_ << ']';
}

void MockGenerator::EmitInterface(const frontend::Interface& declaration) {
  const auto& origin = program_->origins.at(declaration.name);
  const auto wrapper = "Mock" + origin.name;
  const auto implementation = AllocateName("_" + wrapper + "Object");
  const auto calls = AllocateName("_" + wrapper + "Calls");
  const auto control = AllocateName("_control");
  std::vector<std::string> fields;
  fields.reserve(declaration.methods.size());
  for (std::size_t index = 0; index < declaration.methods.size(); ++index) {
    fields.push_back(AllocateName("_method" + std::to_string(index)));
  }
  const auto interface_type = PublicName(declaration.name, declaration.location);
  body_ << "/// Mock adapter for " << origin.name
        << ". Source: " << RelativeModule(origin.module_uri, directory_) << ':'
        << declaration.location.line << "\n"
        << "export class " << wrapper << " {\n"
        << "private _object: " << interface_type << ";\n"
        << "private _calls: " << calls << ";\n"
        << "private _control: " << sdk_ << ".MockControl;\n"
        << "constructor(session: " << sdk_ << ".MockSession) {\n"
        << "const " << control << " = " << sdk_ << ".MockControl(session);\n"
        << "this._control = " << control << ";\n";
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto& method = declaration.methods.at(index);
    const auto identity =
        RelativeModule(origin.module_uri, api_directory_) + "#" + origin.name + '.' + method.name;
    body_ << "const " << fields.at(index) << " = " << sdk_ << ".MockMethod(" << control << ", "
          << Quote(identity) << ");\n";
  }
  const auto field_arguments = [this, &fields] {
    for (std::size_t index = 0; index < fields.size(); ++index) {
      if (index != 0) {
        body_ << ", ";
      }
      const auto& field = fields.at(index);
      body_ << field << ": " << field;
    }
  };
  const auto object = AllocateName("_object");
  body_ << "const " << object << " = " << implementation << '(';
  field_arguments();
  body_ << ");\nthis._object = " << object << ";\nthis._calls = " << calls << '(';
  field_arguments();
  body_ << ");\n";
  for (std::size_t index = 0; index < fields.size(); ++index) {
    body_ << fields.at(index) << ".bind(" << object << '.' << declaration.methods.at(index).name
          << ");\n";
  }
  body_ << "}\nget object(): " << interface_type << " { return this._object; }\n"
        << "get calls(): " << calls << " { return this._calls; }\n"
        << "get control(): " << sdk_ << ".MockControl { return this._control; }\n}\n";
  EmitAdapter(declaration, fields, AdapterKind::Implementation, implementation);
  EmitAdapter(declaration, fields, AdapterKind::Pattern, calls);
}

void MockGenerator::EmitAdapter(const frontend::Interface& declaration,
                                std::span<const std::string> fields,
                                AdapterKind kind,
                                std::string_view name) {
  const auto& origin = program_->origins.at(declaration.name);
  const auto interface_type = PublicName(declaration.name, declaration.location);
  const bool pattern = kind == AdapterKind::Pattern;
  body_ << "class " << name;
  if (!pattern) {
    body_ << " implements " << interface_type;
  }
  body_ << " {\n";
  for (const auto& field : fields) {
    body_ << field << ": " << sdk_ << ".MockMethod;\n";
  }
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto& method = declaration.methods.at(index);
    body_ << "/// " << (pattern ? "Describes" : "Invokes") << ' ' << origin.name << '.'
          << method.name << ".\n"
          << method.name;
    Parameters(method, kind);
    const auto result = NameType(method.return_type);
    const bool is_void = Expand(method.return_type).name == "void";
    body_ << ": " << (pattern ? sdk_ + ".MockCall" : result.ToString()) << " {\n";
    if (pattern || !is_void) {
      body_ << "return ";
    }
    const auto* const invocation = is_void ? ".invokeVoid(" : ".invoke(";
    body_ << "this." << fields.subspan(index).front() << (pattern ? ".query(" : invocation);
    Arguments(method);
    body_ << ')';
    if (!pattern && !is_void) {
      body_ << " as " << result.ToString();
    }
    body_ << ";\n}\n";
  }
  body_ << "}\n";
}

std::string MockGenerator::Fingerprint(std::span<const frontend::Interface* const> selected) const {
  std::map<std::string, std::string_view> sources;
  for (const auto& source : program_->sources) {
    sources.emplace(RelativeModule(source.path, api_directory_), source.text);
  }
  std::ostringstream content;
  for (const auto& [path, text] : sources) {
    content << path.size() << ':' << path << text.size() << ':' << text;
  }
  for (const auto* declaration : selected) {
    content << declaration->name << '\n';
  }
  content << *frontend::SdkModuleSource("sdk:mock");
  return ContentFingerprint(content.str()).Hex();
}

std::expected<std::string, frontend::Diagnostics> MockGenerator::Generate(
    std::span<const std::string> names) {
  absl::flat_hash_set<std::string> remaining(names.begin(), names.end());
  std::vector<const frontend::Interface*> selected;
  for (const auto& declaration : program_->interfaces) {
    const auto& origin = program_->origins.at(declaration.name);
    if (declaration.location.source_id == 0 &&
        declaration.export_kind == frontend::ExportKind::Exported &&
        remaining.erase(origin.name) != 0) {
      selected.push_back(&declaration);
      identifiers_.insert("Mock" + origin.name);
    }
  }
  for (const auto& name : names) {
    if (remaining.erase(name) != 0) {
      Error({}, "API module does not export interface '" + name + "'");
    }
  }
  if (!diagnostics_.empty()) {
    return std::unexpected(std::move(diagnostics_));
  }
  std::ranges::sort(selected, {}, [](const auto* declaration) {
    return std::pair(declaration->location.line, declaration->location.column);
  });
  sdk_ = Import("sdk:mock");
  for (const auto* declaration : selected) {
    EmitInterface(*declaration);
  }
  if (!diagnostics_.empty()) {
    return std::unexpected(std::move(diagnostics_));
  }
  std::ostringstream source;
  source << "// Generated by vela mock " << kGeneratorVersion << "; SDK contract "
         << kDescriptorVersion << "; input " << Fingerprint(selected) << ".\n";
  for (const auto& [module, name] : imports_) {
    auto path = RelativeModule(module, directory_);
    if (!path.starts_with("sdk:") && !path.starts_with('.')) {
      path.insert(0, "./");
    }
    source << "import " << Quote(path) << " as " << name << ";\n";
  }
  source << body_.str();
  auto formatted = FormatSource(source.str());
  if (!formatted) {
    for (auto& diagnostic : formatted.error()) {
      diagnostic.kind = frontend::DiagnosticKind::Internal;
    }
  }
  return formatted;
}

}  // namespace

std::expected<std::string, frontend::Diagnostics> GenerateMocks(
    const frontend::Program& program,
    std::span<const std::string> interfaces,
    const std::filesystem::path& output) {
  return MockGenerator(program, output).Generate(interfaces);
}

}  // namespace vela::tooling
