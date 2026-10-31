#include "compiler/frontend/specialization.h"

#include <cstddef>
#include <expected>
#include <string_view>
#include <utility>

#include "base/declaration.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_limits.h"

namespace vela::frontend {

std::expected<std::size_t, Diagnostic> Specializer::SpecializeMethod(MethodRequest request) {
  const auto depth = expansion_depth_;
  for (auto& argument : request.arguments) {
    Normalize(argument, {});
  }
  const auto& source = *request.declaration;
  const auto name = SpecializationName(source.name, request.arguments);
  const auto key = std::pair(request.owner, name);
  if (const auto found = method_indices_.find(key); found != method_indices_.end()) {
    return found->second;
  }
  if (depth >= kMaxGenericExpansionDepth) {
    return std::unexpected(Diagnostic{
        .location = request.location,
        .message = "generic expansion depth exceeds the limit of 128; check expanding recursion",
        .source_path = {},
    });
  }
  Function declaration;
  declaration.name = name;
  declaration.application =
      Function::Application{.name = source.name, .arguments = request.arguments};
  methods_.push_back({
      .owner = request.owner,
      .kind = request.kind,
      .function =
          {
              .declaration = std::move(declaration),
              .substitutions = Bind(source.type_parameters, request.arguments, request.location),
              .source = &source,
              .expansion_depth = depth + 1,
              .abstract = nullptr,
          },
  });
  const auto index = methods_.size() - 1;
  method_indices_.emplace(key, index);
  expansion_depths_.emplace(request.owner + '.' + name, depth + 1);
  work_.push_back({.kind = Work::Kind::Method, .index = index});
  Expand();
  expansion_depth_ = depth;
  return index;
}

const Function& Specializer::method(std::size_t index) const {
  return methods_.at(index).function.declaration;
}

std::string_view Specializer::method_owner(std::size_t index) const {
  return methods_.at(index).owner;
}

DeclarationKind Specializer::method_owner_kind(std::size_t index) const {
  return methods_.at(index).kind;
}

const Function* Specializer::method_source(std::size_t index) const {
  return methods_.at(index).function.source;
}

}  // namespace vela::frontend
