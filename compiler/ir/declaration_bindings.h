#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>

#include "base/declaration.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"

namespace vela::ir {

struct QualifiedMemberName final {
  std::string_view owner;
  std::string_view name;
};

[[nodiscard]] std::string QualifiedMemberKey(QualifiedMemberName member);

[[nodiscard]] std::string MethodKey(std::string_view owner,
                                    DeclarationKind kind,
                                    std::string_view name);
[[nodiscard]] std::string MethodKey(std::string_view owner, const frontend::Function& method);
[[nodiscard]] DeclarationKind MethodDeclarationKind(const frontend::Function& method);
[[nodiscard]] DeclarationKind MethodDeclarationKind(const frontend::FunctionDescriptor& method);
enum class InitializerKind : std::uint8_t {
  Global,
  StaticField,
  InstanceField,
  ConstructorFactory,
};
[[nodiscard]] std::string InitializerKey(InitializerKind kind, std::string_view name);

struct ClassDeclarations final {
  DeclarationId declaration;
  absl::flat_hash_map<std::string, DeclarationId> fields;
  absl::flat_hash_map<std::string, DeclarationId> properties;
};

// 只在准备阶段借用 AST；Lowerer 将身份值移入拥有源码和布局的 IR。
struct DeclarationBindings final {
  std::uint64_t revision = 0;
  absl::flat_hash_map<const frontend::Function*, FunctionIdentity> functions;
  absl::flat_hash_map<const void*, DeclarationId> attribute_targets;
  absl::flat_hash_map<std::string, DeclarationId> named_functions;
  absl::flat_hash_map<std::string, ClassDeclarations> classes;
  absl::flat_hash_map<std::string, DeclarationId> nominals;
  absl::flat_hash_map<std::string, DeclarationId> globals;
  absl::flat_hash_map<std::string, DeclarationId> selectors;
  absl::flat_hash_map<std::string, absl::flat_hash_map<std::string, DeclarationId>> enum_members;
};

[[nodiscard]] DeclarationBindings BindDeclarations(const frontend::Program& source,
                                                   const frontend::SemanticModel& model,
                                                   frontend::DeclarationIds& identities,
                                                   std::uint64_t revision);

}  // namespace vela::ir
