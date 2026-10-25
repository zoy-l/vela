#include "compiler/type_constraints.h"

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::compiler {

std::vector<runtime::TypeConstraint> CreateTypeConstraints(
    const ir::Program& program,
    const frontend::SemanticModel& model,
    std::span<runtime::ClassLayout* const> classes,
    std::span<const runtime::ArrayLayout* const> arrays,
    std::span<const runtime::CollectionLayout* const> collections) {
  absl::flat_hash_map<std::string, const runtime::ClassLayout*> class_layouts;
  for (std::size_t index = 0; index < program.classes.size(); ++index) {
    class_layouts.emplace(program.classes.at(index).name, classes.subspan(index).front());
  }
  absl::flat_hash_map<std::string, const runtime::ArrayLayout*> array_layouts;
  for (std::size_t index = 0; index < program.array_types.size(); ++index) {
    array_layouts.emplace(program.array_types.at(index).name, arrays.subspan(index).front());
  }
  std::vector<runtime::TypeConstraint> constraints;
  absl::flat_hash_map<std::string, const runtime::CollectionLayout*> collection_layouts;
  for (std::size_t index = 0; index < program.collection_types.size(); ++index) {
    collection_layouts.emplace(program.collection_types.at(index).name,
                               collections.subspan(index).front());
  }
  for (const auto& type : program.runtime_types) {
    const auto name = type.nullable ? type.name.substr(0, type.name.size() - 1) : type.name;
    runtime::TypeConstraint target;
    target.nullable = type.nullable;
    using Kind = frontend::TypeDescriptor::Kind;
    switch (type.kind) {
      case Kind::Bool:
        target.kind = runtime::ValueKind::Boolean;
        break;
      case Kind::Int:
      case Kind::Enum:
        target.kind = runtime::ValueKind::Integer;
        break;
      case Kind::Double:
        target.kind = runtime::ValueKind::Double;
        break;
      case Kind::String:
        target.kind = runtime::ValueKind::String;
        break;
      case Kind::Class:
      case Kind::Expected:
        target.kind = runtime::ValueKind::Object;
        target.instance = class_layouts.at(name);
        if (type.kind == Kind::Expected) {
          target.object_kind = runtime::ObjectKind::Expected;
        }
        break;
      case Kind::Interface:
        target.kind = runtime::ValueKind::Object;
        target.interface = model.interfaces.at(name).index;
        break;
      case Kind::Array:
        target.kind = runtime::ValueKind::Object;
        target.object_kind = runtime::ObjectKind::Array;
        target.array = array_layouts.at(name);
        break;
      case Kind::MutableString:
        target.kind = runtime::ValueKind::Object;
        target.object_kind = runtime::ObjectKind::MutableString;
        break;
      case Kind::Function:
        target.kind = runtime::ValueKind::Object;
        target.callable = true;
        break;
      case Kind::Set:
      case Kind::Map:
        target.kind = runtime::ValueKind::Object;
        target.object_kind = runtime::ObjectKind::Collection;
        target.collection = collection_layouts.at(name);
        break;
      case Kind::Invalid:
      case Kind::Null:
      case Kind::Void:
        std::unreachable();
    }
    constraints.push_back(target);
  }
  return constraints;
}

}  // namespace vela::compiler
