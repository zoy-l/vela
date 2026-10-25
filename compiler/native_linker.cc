#include "compiler/native_linker.h"

#include <cstddef>
#include <span>
#include <string>
#include <utility>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"
#include "runtime/native_function.h"

namespace vela::compiler {
namespace {

auto MatchesNativeType(const frontend::TypeDescriptor& type, runtime::NativeType native) -> bool {
  using Kind = frontend::TypeDescriptor::Kind;
  if (native == runtime::NativeType::NullableString) {
    return type.kind == Kind::String && type.nullable;
  }
  if (type.nullable) {
    return false;
  }
  switch (native) {
    case runtime::NativeType::Void:
      return type.kind == Kind::Void;
    case runtime::NativeType::Boolean:
      return type.kind == Kind::Bool;
    case runtime::NativeType::Integer:
      return type.kind == Kind::Int;
    case runtime::NativeType::Double:
      return type.kind == Kind::Double;
    case runtime::NativeType::String:
      return type.kind == Kind::String;
    case runtime::NativeType::NullableString:
      std::unreachable();
  }
  std::unreachable();
}

auto SignatureMatches(const frontend::FunctionDescriptor& signature,
                      const runtime::NativeFunction& native) -> bool {
  const auto parameters = native.parameters();
  if (signature.parameters.size() != parameters.size() ||
      !MatchesNativeType(signature.return_type, native.result_type())) {
    return false;
  }
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (!MatchesNativeType(signature.parameters.at(index).type,
                           parameters.subspan(index).front())) {
      return false;
    }
  }
  return true;
}

}  // namespace

auto CheckNativeBindings(const frontend::Program& program,
                         const frontend::SemanticModel& model,
                         const runtime::Isolate& isolate) -> frontend::Diagnostics {
  frontend::Diagnostics diagnostics;
  const auto check = [&](const frontend::Function& function,
                         const frontend::FunctionDescriptor& signature) -> void {
    if (function.native_symbol.empty()) {
      return;
    }
    const auto binding = isolate.FindNative(function.native_symbol);
    std::string message;
    if (binding == nullptr) {
      message = "unregistered native function '" + function.native_symbol + "'";
    } else if (!SignatureMatches(signature, *binding)) {
      message = "external declaration does not match native signature for '" +
                function.native_symbol + "'";
    } else {
      return;
    }
    diagnostics.push_back({
        .location = function.location,
        .message = std::move(message),
        .source_path = program.sources.empty()
                           ? std::string()
                           : program.sources.at(function.location.source_id).path,
    });
  };
  for (const auto& function : program.functions) {
    check(function, model.functions.at(function.name));
  }
  for (const auto& owner : program.classes) {
    const auto& methods = model.classes.at(owner.name).methods;
    for (const auto& method : owner.methods) {
      check(method, methods.at(method.name));
    }
  }
  return diagnostics;
}

}  // namespace vela::compiler
