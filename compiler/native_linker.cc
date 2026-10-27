#include "compiler/native_linker.h"

#include <cstddef>
#include <span>
#include <string>
#include <utility>

#include "base/callable.h"
#include "base/layout_description.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "runtime/native_function.h"

namespace vela::compiler {
namespace {

bool MatchesNativeType(const layout::Type& type, runtime::NativeType native) {
  using Kind = layout::Type::Kind;
  if (native == runtime::NativeType::NullableString) {
    return type.kind == Kind::String && type.nullable;
  }
  if (type.nullable) {
    return false;
  }
  switch (native) {
    case runtime::NativeType::Void:
      return false;
    case runtime::NativeType::Boolean:
      return type.kind == Kind::Boolean;
    case runtime::NativeType::Integer:
      return type.kind == Kind::Integer;
    case runtime::NativeType::Double:
      return type.kind == Kind::Double;
    case runtime::NativeType::String:
      return type.kind == Kind::String;
    case runtime::NativeType::NullableString:
      std::unreachable();
    case runtime::NativeType::Future:
      return type.kind == Kind::Future;
    case runtime::NativeType::CancellationToken:
      return type.kind == Kind::CancellationToken;
  }
  std::unreachable();
}

bool SignatureMatches(const CallSignature& signature,
                      std::span<const layout::Type> types,
                      const runtime::NativeFunction& native) {
  const auto parameters = native.parameters();
  const auto result_type = native.result_type();
  const bool matches_result =
      signature.result ? MatchesNativeType(types.subspan(*signature.result).front(), result_type)
                       : result_type == runtime::NativeType::Void;
  if (signature.parameters.size() != parameters.size() || !matches_result) {
    return false;
  }
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (!MatchesNativeType(types.subspan(signature.parameters.at(index).type).front(),
                           parameters.subspan(index).front())) {
      return false;
    }
  }
  return true;
}

}  // namespace

frontend::Diagnostics CheckNativeBindings(const ir::Program& program,
                                          const runtime::Isolate& isolate) {
  frontend::Diagnostics diagnostics;
  for (const auto& function : program.functions) {
    if (function.native_symbol.empty()) {
      continue;
    }
    const auto binding = isolate.FindNative(function.native_symbol);
    std::string message;
    if (binding == nullptr) {
      message = "unregistered native function '" + function.native_symbol + "'";
    } else if (!SignatureMatches(*function.signature, program.layouts.types, *binding)) {
      message = "external declaration does not match native signature for '" +
                function.native_symbol + "'";
    } else {
      continue;
    }
    diagnostics.push_back({
        .location = function.location,
        .message = std::move(message),
        .source_path = program.source_paths.empty()
                           ? std::string()
                           : program.source_paths.at(function.location.source_id),
    });
  }
  return diagnostics;
}

}  // namespace vela::compiler
