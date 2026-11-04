#include "compiler/frontend/typing/semantic_model.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

Diagnostics BuildJSONDecodePlans(SemanticModel& model) {
  using Kind = TypeDescriptor::Kind;
  struct Work final {
    std::uint32_t index;
    SourceLocation location;
    std::string path;
  };
  Diagnostics diagnostics;
  absl::flat_hash_map<std::string, std::uint32_t> indices;
  std::deque<Work> work;
  const auto intern = [&](const TypeDescriptor& type, SourceLocation location, std::string path) {
    const auto index = static_cast<std::uint32_t>(model.json_decode_plans.size());
    const auto [found, inserted] = indices.try_emplace(std::string(type.name), index);
    if (inserted) {
      model.json_decode_plans.push_back({.type = type, .element = {}, .parameters = {}});
      work.push_back({.index = index, .location = location, .path = std::move(path)});
    }
    return found->second;
  };
  std::vector<std::string> entries;
  for (const auto& [name, request] : model.json_decode_requests) {
    static_cast<void>(request);
    entries.push_back(name);
  }
  std::ranges::sort(entries);
  for (const auto& name : entries) {
    const auto& request = model.json_decode_requests.at(name);
    model.json_decode_entries.emplace(name, intern(request.type, request.location, "$"));
  }
  while (!work.empty()) {
    // intern 会增长节点和队列，处理当前节点时不跨越插入持有其引用。
    const auto current = std::move(work.front());
    work.pop_front();
    const auto type = model.json_decode_plans.at(current.index).type;
    auto underlying = type;
    if (type.nullable) {
      const auto name = type.kind == Kind::Function ? type.name.substr(1, type.name.size() - 3)
                                                    : type.name.substr(0, type.name.size() - 1);
      underlying = model.types.at(std::string(name));
      const auto child = intern(underlying, current.location, current.path);
      model.json_decode_plans.at(current.index).element = child;
      continue;
    }
    bool supported = !type.is_abstract;
    switch (type.kind) {
      case Kind::Bool:
      case Kind::Int:
      case Kind::Double:
      case Kind::String:
      case Kind::Dynamic:
        break;
      case Kind::Array: {
        const auto child =
            intern(model.array_elements.at(type.name), current.location, current.path + "[]");
        model.json_decode_plans.at(current.index).element = child;
        break;
      }
      case Kind::Map: {
        const auto& arguments = model.generic_arguments.at(type.name);
        supported = arguments.front().kind == Kind::String && !arguments.front().nullable;
        const auto child = intern(arguments.back(), current.location, current.path + "[value]");
        model.json_decode_plans.at(current.index).element = child;
        break;
      }
      case Kind::Class: {
        const auto& owner = model.classes.at(type.name);
        if (!owner.declaration->native_key.empty()) {
          supported = false;
          break;
        }
        supported =
            owner.constructor.visibility == Visibility::Public && !IsStandardName(type.name);
        std::vector<JSONDecodeParameter> parameters;
        parameters.reserve(owner.constructor.parameters.size());
        for (const auto& parameter : owner.constructor.parameters) {
          parameters.push_back({
              .name = parameter.name,
              .node = intern(parameter.type, current.location, current.path + '.' + parameter.name),
              .optional = owner.constructor.optional_parameters.contains(parameter.name),
          });
        }
        model.json_decode_plans.at(current.index).parameters = std::move(parameters);
        break;
      }
      default:
        supported = false;
        break;
    }
    if (!supported) {
      diagnostics.push_back({
          .location = current.location,
          .message = "JSON cannot decode type '" + std::string(type.name) + "' at " + current.path,
          .source_path = {},
      });
    }
  }
  return diagnostics;
}

}  // namespace vela::frontend
