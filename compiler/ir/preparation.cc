#include "compiler/ir/preparation.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "base/declaration.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/declaration_ids.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/specialization.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/declaration_bindings.h"
#include "compiler/ir/layout_description.h"
#include "compiler/ir/lowerer.h"
#include "compiler/ir/optimizer.h"
#include "compiler/ir/persistent_ids.h"

namespace vela::ir {

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
    absl::flat_hash_map<DeclarationId, std::vector<std::string>> type_parameters;
    absl::flat_hash_map<DeclarationId, std::vector<std::string>> enum_members;
    for (const auto& enumeration : source.enums) {
      auto& members = enum_members[bindings.nominals.at(enumeration.name)];
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
    const auto records = options.declarations->records();
    for (std::size_t index = 0; index < records.size(); ++index) {
      const auto& record = records[index];
      if (!record.retired) {
        const DeclarationId identity{static_cast<std::uint64_t>(index)};
        const auto parameters = type_parameters.find(identity);
        const auto members = enum_members.find(identity);
        program.declarations.push_back({
            .identity = identity,
            .owner = record.key.owner,
            .kind = record.key.kind,
            .name = record.key.name,
            .type_parameters = parameters == type_parameters.end() ? std::vector<std::string>{}
                                                                   : parameters->second,
            .enum_members =
                members == enum_members.end() ? std::vector<std::string>{} : members->second,
        });
      }
    }
    program.interface_declarations.resize(program.layouts.interfaces.size());
    for (const auto& interface : source.interfaces) {
      program.interface_declarations.at(model.interfaces.at(interface.name).index) =
          bindings.nominals.at(interface.name);
    }
    program.global_declarations.resize(program.global_initializers.size());
    program.global_flags.resize(program.global_initializers.size());
    absl::flat_hash_map<DeclarationId, std::size_t> global_slots;
    for (std::size_t index = 0; index < program.global_initializers.size(); ++index) {
      const auto function = program.global_initializers[index];
      const auto initializer = program.functions.at(function).identity.declaration;
      program.global_declarations[index] =
          records[static_cast<std::size_t>(initializer.value)].key.owner;
      global_slots.emplace(program.global_declarations[index], index);
    }
    for (const auto& global : source.globals) {
      if (global.binding == frontend::BindingKind::Constant) {
        program.global_flags[global_slots.at(bindings.globals.at(global.name))] =
            static_cast<std::uint32_t>(GlobalBinding::Constant);
      }
    }
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
