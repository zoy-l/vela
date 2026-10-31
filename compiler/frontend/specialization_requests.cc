#include "compiler/frontend/specialization.h"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/declaration.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/declaration_ids.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {
namespace {

using OriginKey = std::tuple<std::string, DeclarationKind, std::string>;

class SpecializationRequests final {
 public:
  SpecializationRequests(const Program& program, const DeclarationIds& identities);
  std::expected<std::vector<GenericSpecialization>, Diagnostics> Run(
      std::span<const DeclarationId> declarations);

 private:
  [[nodiscard]] const std::string* Name(DeclarationId declaration) const;
  [[nodiscard]] std::optional<TypeName> Type(const TypeIdentity& identity) const;
  std::expected<std::optional<GenericSpecialization>, Diagnostic> Request(
      const DeclarationRecord& record) const;
  std::span<const DeclarationRecord> records_;
  absl::flat_hash_map<OriginKey, std::string> names_;
};

SpecializationRequests::SpecializationRequests(const Program& program,
                                               const DeclarationIds& identities)
    : records_(identities.records()) {
  for (const auto& [name, origin] : program.origins) {
    names_.emplace(OriginKey{origin.module_uri, origin.kind, origin.name}, name);
  }
}

const std::string* SpecializationRequests::Name(DeclarationId declaration) const {
  const auto& record = records_.subspan(static_cast<std::size_t>(declaration.value)).front();
  if (record.retired) {
    return nullptr;
  }
  if (!record.key.arguments.empty()) {
    return Name(record.key.owner);
  }
  const auto& module = records_.subspan(static_cast<std::size_t>(record.key.owner.value)).front();
  const auto found = names_.find(OriginKey{module.key.name, record.key.kind, record.key.name});
  return found == names_.end() ? nullptr : &found->second;
}

std::optional<TypeName> SpecializationRequests::Type(const TypeIdentity& identity) const {
  TypeName type;
  using Kind = TypeIdentity::Kind;
  switch (identity.kind) {
    case Kind::Declared: {
      const auto* name = Name(identity.declaration);
      if (name == nullptr) {
        return std::nullopt;
      }
      type.name = *name;
      const auto& record =
          records_.subspan(static_cast<std::size_t>(identity.declaration.value)).front();
      type.kind = record.key.arguments.empty() ? TypeName::Kind::Named : TypeName::Kind::Generic;
      for (const auto& argument : record.key.arguments) {
        auto child = Type(argument);
        if (!child) {
          return std::nullopt;
        }
        type.arguments.push_back(std::move(*child));
      }
      return type;
    }
    case Kind::Builtin:
      type.kind = TypeName::Kind::Named;
      break;
    case Kind::Array:
      type.kind = TypeName::Kind::Array;
      break;
    case Kind::Nullable:
      type.kind = TypeName::Kind::Nullable;
      break;
    case Kind::Function:
      type.kind = TypeName::Kind::Function;
      break;
    case Kind::Generic:
      type.kind = TypeName::Kind::Generic;
      break;
  }
  type.name = identity.builtin;
  for (const auto& argument : identity.arguments) {
    auto child = Type(argument);
    if (!child) {
      return std::nullopt;
    }
    type.arguments.push_back(std::move(*child));
  }
  return type;
}

std::expected<std::optional<GenericSpecialization>, Diagnostic> SpecializationRequests::Request(
    const DeclarationRecord& record) const {
  const bool method = record.key.kind == DeclarationKind::Method ||
                      record.key.kind == DeclarationKind::StaticMethod ||
                      record.key.kind == DeclarationKind::Selector;
  auto owner = record.key.owner;
  if (method) {
    owner = records_.subspan(static_cast<std::size_t>(owner.value)).front().key.owner;
  }
  const auto* name = Name(owner);
  // 模板本身删除时仍遵循目标已删除语义，不为旧声明复活新身份。
  if (name == nullptr) {
    return std::nullopt;
  }
  GenericSpecialization request{.name = *name, .arguments = {}};
  const auto convert =
      [&](std::span<const TypeIdentity> arguments) -> std::optional<std::vector<TypeName>> {
    std::vector<TypeName> result;
    for (const auto& argument : arguments) {
      auto type = Type(argument);
      if (!type) {
        return std::nullopt;
      }
      result.push_back(std::move(*type));
    }
    return result;
  };
  const auto& owner_record = records_.subspan(static_cast<std::size_t>(owner.value)).front();
  auto arguments = convert(record.key.arguments);
  auto owner_arguments =
      method ? convert(owner_record.key.arguments) : std::optional(std::vector<TypeName>{});
  if (!arguments || !owner_arguments) {
    return std::unexpected(Diagnostic{
        .location = {},
        .message = "retained generic specialization depends on a deleted type",
        .source_path = {},
    });
  }
  if (method) {
    request.arguments = std::move(*owner_arguments);
    request.method =
        Function::Application{.name = record.key.name, .arguments = std::move(*arguments)};
  } else {
    request.arguments = std::move(*arguments);
  }
  return request;
}

std::expected<std::vector<GenericSpecialization>, Diagnostics> SpecializationRequests::Run(
    std::span<const DeclarationId> declarations) {
  std::vector<GenericSpecialization> requests;
  absl::flat_hash_set<DeclarationId> visited;
  std::vector<DeclarationId> pending(declarations.begin(), declarations.end());
  while (!pending.empty()) {
    const auto identity = pending.back();
    pending.pop_back();
    if (identity == DeclarationId{} || !visited.insert(identity).second) {
      continue;
    }
    if (identity.value >= records_.size()) {
      return std::unexpected(Diagnostics{
          {
              .location = {},
              .message = "unknown retained declaration identity",
              .source_path = {},
          },
      });
    }
    const auto& record = records_.subspan(static_cast<std::size_t>(identity.value)).front();
    pending.push_back(record.key.owner);
    if (!record.retired && !record.key.arguments.empty()) {
      auto request = Request(record);
      if (!request) {
        return std::unexpected(Diagnostics{std::move(request.error())});
      }
      if (*request) {
        requests.push_back(std::move(**request));
      }
    }
  }
  return requests;
}

}  // namespace

std::expected<std::vector<GenericSpecialization>, Diagnostics> RetainedSpecializations(
    const Program& program,
    const DeclarationIds& identities,
    std::span<const DeclarationId> declarations) {
  return SpecializationRequests(program, identities).Run(declarations);
}

}  // namespace vela::frontend
