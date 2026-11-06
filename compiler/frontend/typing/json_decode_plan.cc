#include "compiler/frontend/typing/semantic_model.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
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
  };
  struct Path final {
    std::optional<std::uint32_t> parent;
    std::string_view suffix;
    std::string_view name;
  };
  Diagnostics diagnostics;
  absl::flat_hash_map<std::string_view, std::uint32_t> indices;
  std::vector<Path> paths;
  std::deque<Work> work;
  const auto intern = [&](const TypeDescriptor& type, SourceLocation location, Path path) {
    const auto index = static_cast<std::uint32_t>(model.json_decode_plans.size());
    const auto [found, inserted] = indices.try_emplace(type.name, index);
    if (inserted) {
      model.json_decode_plans.push_back({.type = type, .element = {}, .parameters = {}});
      paths.push_back(path);
      work.push_back({.index = index, .location = location});
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
    model.json_decode_entries.emplace(name, intern(request.type, request.location, {}));
  }
  while (!work.empty()) {
    // intern 会增长节点和队列，处理当前节点时不跨越插入持有其引用。
    const auto current = work.front();
    work.pop_front();
    const auto type = model.json_decode_plans.at(current.index).type;
    auto underlying = type;
    if (type.nullable) {
      underlying = model.types.at(NonNullableTypeName(type));
      const auto child =
          intern(underlying, current.location, {.parent = current.index, .suffix = {}, .name = {}});
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
        const auto child = intern(model.array_elements.at(type.name), current.location,
                                  {.parent = current.index, .suffix = "[]", .name = {}});
        model.json_decode_plans.at(current.index).element = child;
        break;
      }
      case Kind::Map: {
        const auto& arguments = model.generic_arguments.at(type.name);
        supported = arguments.front().kind == Kind::String && !arguments.front().nullable;
        const auto child = intern(arguments.back(), current.location,
                                  {.parent = current.index, .suffix = "[value]", .name = {}});
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
              .node = intern(parameter.type, current.location,
                             {.parent = current.index, .suffix = ".", .name = parameter.name}),
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
      // 成功路径只保存父节点和借用的段；完整路径仅在生成诊断时渲染。
      std::vector<std::uint32_t> chain;
      for (std::optional<std::uint32_t> node = current.index; node; node = paths[*node].parent) {
        chain.push_back(*node);
      }
      std::string path = "$";
      for (unsigned int& node : std::views::reverse(chain)) {
        const auto& segment = paths[node];
        path += segment.suffix;
        path += segment.name;
      }
      diagnostics.push_back({
          .location = current.location,
          .message = "JSON cannot decode type '" + std::string(type.name) + "' at " + path,
          .source_path = {},
      });
    }
  }
  return diagnostics;
}

}  // namespace vela::frontend
