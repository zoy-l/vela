#include "compiler/ir/preparation.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/declaration_ids.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/specialization.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/declaration_bindings.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/layout_description.h"
#include "compiler/ir/lowerer.h"
#include "compiler/ir/optimizer.h"

namespace vela::ir {
namespace {

using ClassIndex = absl::flat_hash_map<std::string, const Class*>;
using DeclarationKinds = absl::flat_hash_map<DeclarationId, DeclarationKind>;

void BindTypeDependencies(Program& program,
                          const frontend::SemanticModel& model,
                          const DeclarationBindings& bindings) {
  using Kind = frontend::TypeDescriptor::Kind;
  absl::flat_hash_map<std::string, std::vector<DeclarationId>> cache;
  const auto collect =
      [&](const auto& self,
          const frontend::TypeDescriptor& type) -> const std::vector<DeclarationId>& {
    auto key = type.name;
    if (type.nullable) {
      key = type.kind == Kind::Function ? key.substr(1, key.size() - 3)
                                        : key.substr(0, key.size() - 1);
    }
    if (const auto found = cache.find(key); found != cache.end()) {
      return found->second;
    }
    std::vector<DeclarationId> result;
    const auto append = [&](const auto& child) {
      const auto& dependencies = self(self, child);
      result.insert(result.end(), dependencies.begin(), dependencies.end());
    };
    switch (type.kind) {
      case Kind::Class:
      case Kind::Enum:
      case Kind::Interface: {
        const auto nominal = bindings.nominals.find(key);
        // $Cell 等 lowering 内部类型只属于函数版本，不请求源码模板。
        if (nominal != bindings.nominals.end()) {
          result.push_back(nominal->second);
        }
        break;
      }
      case Kind::Array:
        append(model.array_elements.at(key));
        break;
      case Kind::Function: {
        const auto& signature = model.function_types.at(key);
        append(signature.return_type);
        for (const auto& parameter : signature.parameters) {
          append(parameter.type);
        }
        break;
      }
      case Kind::Future:
      case Kind::Completer:
      case Kind::Set:
      case Kind::Map:
        for (const auto& argument : model.generic_arguments.at(key)) {
          append(argument);
        }
        break;
      default:
        break;
    }
    return cache.emplace(key, std::move(result)).first->second;
  };
  for (auto& function : program.functions) {
    absl::flat_hash_set<DeclarationId> dependencies;
    const auto append = [&](const auto& type) {
      const auto& types = collect(collect, type);
      dependencies.insert(types.begin(), types.end());
    };
    append(function.return_type);
    for (const auto& type : function.value_types) {
      append(type);
    }
    function.type_dependencies.assign(dependencies.begin(), dependencies.end());
  }
}

void BindFieldIdentity(Instruction& instruction,
                       const Function& function,
                       const ClassIndex& classes) {
  const auto receiver = instruction.opcode == Opcode::GetField ? instruction.lhs : instruction.dst;
  const auto& type = function.value_types.at(receiver);
  const auto name = type.nullable ? type.name.substr(0, type.name.size() - 1) : type.name;
  const auto owner = classes.find(name);
  if (owner != classes.end() && owner->second->kind == Class::Kind::Instance &&
      !owner->second->callable) {
    instruction.field_declaration = owner->second->fields.at(instruction.index).declaration;
  }
}

void BindCallReceiver(Instruction& instruction,
                      const Function& function,
                      const Program& program,
                      const ClassIndex& classes,
                      const DeclarationKinds& kinds) {
  if (instruction.binding_receiver) {
    return;
  }
  const auto& target = program.functions.at(instruction.index);
  const auto declaration = kinds.find(target.identity.declaration);
  if (declaration == kinds.end()) {
    return;
  }
  const auto kind = declaration->second;
  if (kind != DeclarationKind::Method && kind != DeclarationKind::Getter &&
      kind != DeclarationKind::Setter) {
    return;
  }
  const auto& type = function.value_types.at(instruction.lhs);
  const auto name = type.nullable ? type.name.substr(0, type.name.size() - 1) : type.name;
  const auto owner = classes.find(name);
  if (owner != classes.end()) {
    instruction.binding_receiver = owner->second->declaration;
  }
}

void BindReloadableAccesses(Program& program) {
  ClassIndex classes;
  for (const auto& owner : program.classes) {
    if (owner.declaration != DeclarationId{}) {
      classes.emplace(owner.name, &owner);
    }
  }
  DeclarationKinds kinds;
  for (const auto& declaration : program.declarations) {
    kinds.emplace(declaration.identity, declaration.kind);
  }
  for (auto& function : program.functions) {
    for (auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::GetField || instruction.opcode == Opcode::SetField) {
        BindFieldIdentity(instruction, function, classes);
      } else if (instruction.opcode == Opcode::Call) {
        BindCallReceiver(instruction, function, program, classes, kinds);
      }
    }
  }
}

void DescribeDeclarations(Program& program,
                          const frontend::Program& source,
                          const DeclarationBindings& bindings,
                          std::span<const frontend::DeclarationRecord> records) {
  absl::flat_hash_map<DeclarationId, std::vector<std::string>> type_parameters;
  absl::flat_hash_map<DeclarationId, std::vector<std::string>> enum_members;
  for (const auto& enumeration : source.enums) {
    auto& members = enum_members.try_emplace(bindings.nominals.at(enumeration.name)).first->second;
    for (const auto& member : enumeration.members) {
      members.push_back(member.name);
    }
  }
  for (const auto& [name, origin] : source.origins) {
    if (origin.arguments.empty() && !origin.type_parameters.empty()) {
      const auto identity = origin.kind == DeclarationKind::Function
                                ? bindings.named_functions.at(name)
                                : bindings.nominals.at(name);
      type_parameters.emplace(identity, origin.type_parameters);
    }
  }
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& record = records.subspan(index).front();
    if (!record.retired) {
      const DeclarationId identity{static_cast<std::uint64_t>(index)};
      const auto parameters = type_parameters.find(identity);
      const auto members = enum_members.find(identity);
      program.declarations.push_back({
          .identity = identity,
          .owner = record.key.owner,
          .kind = record.key.kind,
          .name = record.key.name,
          .type_parameters =
              parameters == type_parameters.end() ? std::vector<std::string>{} : parameters->second,
          .enum_members =
              members == enum_members.end() ? std::vector<std::string>{} : members->second,
          .definition = record.definition,
      });
    }
  }
}

void BindGlobalIdentities(Program& program,
                          const frontend::Program& source,
                          const DeclarationBindings& bindings,
                          std::span<const frontend::DeclarationRecord> records) {
  program.global_declarations.resize(program.global_initializers.size());
  program.global_flags.resize(program.global_initializers.size());
  absl::flat_hash_map<DeclarationId, std::size_t> global_slots;
  for (std::size_t index = 0; index < program.global_initializers.size(); ++index) {
    const auto function = program.global_initializers.at(index);
    const auto initializer = program.functions.at(function).identity.declaration;
    const auto declaration =
        records.subspan(static_cast<std::size_t>(initializer.value)).front().key.owner;
    program.global_declarations.at(index) = declaration;
    global_slots.emplace(declaration, index);
  }
  for (const auto& global : source.globals) {
    if (global.binding == frontend::BindingKind::Constant) {
      program.global_flags.at(global_slots.at(bindings.globals.at(global.name))) =
          static_cast<std::uint32_t>(GlobalFlag::Constant);
    }
  }
  for (const auto& owner : source.classes) {
    for (const auto& field : owner.fields) {
      if (field.storage == frontend::MemberStorage::Static &&
          field.visibility == frontend::Visibility::Private) {
        const auto identity = bindings.globals.at(owner.name + '.' + field.name);
        program.global_flags.at(global_slots.at(identity)) |=
            static_cast<std::uint32_t>(GlobalFlag::Private);
      }
    }
  }
}

}  // namespace

LoweringResult PrepareProgram(const frontend::Program& source,
                              const frontend::SemanticModel& model,
                              const PreparationOptions& options) {
  std::optional<DeclarationBindings> declarations;
  if (options.declarations != nullptr) {
    declarations = BindDeclarations(source, model, *options.declarations, options.revision);
  }
  auto lowered = Lowerer(source, model, options.persistent_slots,
                         declarations ? &*declarations : nullptr, options.native_mode)
                     .Lower();
  if (!lowered.Succeeded()) {
    return lowered;
  }
  auto& program = lowered.program;
  program.source_paths.reserve(source.sources.size());
  program.source_texts.reserve(source.sources.size());
  for (const auto& file : source.sources) {
    program.source_paths.push_back(file.path);
    program.source_texts.emplace_back(file.text);
  }
  program.layouts = DescribeLayouts(program, model);
  if (declarations) {
    const auto& bindings = *declarations;
    program.revision = bindings.revision;
    const auto records = options.declarations->records();
    DescribeDeclarations(program, source, bindings, records);
    BindGlobalIdentities(program, source, bindings, records);
    program.interface_declarations.resize(program.layouts.interfaces.size());
    for (const auto& interface : source.interfaces) {
      program.interface_declarations.at(model.interfaces.at(interface.name).index) =
          bindings.nominals.at(interface.name);
    }
  }
  if (options.native_mode == NativeProgramMode::Reloadable) {
    BindTypeDependencies(program, model, *declarations);
    BindReloadableAccesses(program);
  }
  for (auto& function : program.functions) {
    if (function.native_symbol.empty()) {
      Optimize(function, program.functions);
    }
  }
  return lowered;
}

std::expected<Program, frontend::Diagnostics> PrepareParsed(frontend::ParseResult parsed) {
  if (!parsed.diagnostics.empty()) {
    return std::unexpected(std::move(parsed.diagnostics));
  }
  auto specialized = frontend::SpecializeGenerics(parsed.program);
  if (!specialized.empty()) {
    return std::unexpected(std::move(specialized));
  }
  auto checked = frontend::TypeChecker().Check(parsed.program);
  if (!checked.diagnostics.empty()) {
    return std::unexpected(std::move(checked.diagnostics));
  }
  auto lowered = PrepareProgram(parsed.program, checked.model);
  if (!lowered.Succeeded()) {
    return std::unexpected(std::move(lowered.diagnostics));
  }
  return std::move(lowered.program);
}

}  // namespace vela::ir
