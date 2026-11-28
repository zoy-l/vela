#include "benchmarks/production/json.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <yyjson.h>

#include "base/json.h"
#include "benchmarks/production/allocation.h"
#include "benchmarks/production/contract.h"
#include "benchmarks/production/measurement.h"

namespace vela::benchmarks::production {

template <bool Counted>
class JsonReference final {
 public:
  explicit JsonReference(const Case& problem) : problem_(problem) {}

  std::int64_t Run() {
    yyjson_alc allocator{};
    if constexpr (Counted) {
      allocator = {
          .malloc = AllocateCounted,
          .realloc = ReallocateCounted,
          .free = FreeCounted,
          .ctx = ActiveAllocationLedger(),
      };
    }
    auto document = JsonDocument::Parse(problem_.json.input, YYJSON_READ_NOFLAG,
                                        Counted ? &allocator : nullptr);
    if (!document) {
      return 0;
    }
    auto* root = document->root();
    if (problem_.kind == Kind::JsonScalar) {
      return yyjson_is_sint(root) || yyjson_is_uint(root) ? yyjson_get_sint(root) : 0;
    }
    String path = "$";
    if (problem_.kind == Kind::JsonNested) {
      root = GetMutableRoot(root, path);
      if (root == nullptr) {
        return 0;
      }
    }
    const bool object = problem_.kind == Kind::JsonObject;
    if (!object && !yyjson_is_arr(root)) {
      return 0;
    }
    const auto error = ValidateBatch(root, path);
    if (error) {
      return IsExpectedError(*error, *document) ? kSuccess : 0;
    }
    if (!problem_.json.code.empty()) {
      return 0;
    }
    return Materialize(root);
  }

 private:
  std::int64_t Materialize(yyjson_val* root) const {
    const bool object = problem_.kind == Kind::JsonObject;
    std::vector<Item, Allocator<Item, Counted>> items;
    items.reserve(object ? 1 : yyjson_arr_size(root));
    std::int64_t calls = 0;
    const auto materialize = [&](yyjson_val* item) {
      auto value = yyjson_get_sint(GetJsonField(item, "value"));
      const auto text = GetJsonString(item, "text");
      if (problem_.kind == Kind::JsonConstruct) {
        ++calls;
        value += static_cast<std::int64_t>(text.size());
      }
      items.push_back({value, String(text)});
    };
    if (object) {
      materialize(root);
    } else {
      auto iterator = yyjson_arr_iter_with(root);
      while (auto* item = yyjson_arr_iter_next(&iterator)) {
        materialize(item);
      }
    }
    if (problem_.kind == Kind::JsonConstruct && calls != problem_.size) {
      return 0;
    }
    std::int64_t total = 0;
    std::int64_t rank = 0;
    for (const auto& item : items) {
      if (item.text != "Vela") {
        return 0;
      }
      total += ++rank * (item.value + static_cast<std::int64_t>(item.text.size()));
    }
    return total;
  }

  using String = std::basic_string<char, std::char_traits<char>, Allocator<char, Counted>>;
  struct Item final {
    std::int64_t value;
    String text;
  };
  struct Error final {
    std::string_view code;
    String path;
    std::string_view expected;
    std::string_view actual;
    const yyjson_val* node{};
  };
  static yyjson_val* GetMutableRoot(yyjson_val* root, String& path) {
    for (int depth = 0; depth < 2; ++depth) {
      if (!yyjson_is_obj(root) || yyjson_obj_size(root) != 1) {
        return nullptr;
      }
      root = yyjson_obj_get(root, "value");
      path += ".value";
    }
    return root;
  }
  static std::string_view Type(const yyjson_val* value) {
    if (yyjson_is_bool(value)) {
      return "bool";
    }
    if (yyjson_is_str(value)) {
      return "string";
    }
    if (yyjson_is_int(value)) {
      return "int";
    }
    if (yyjson_is_real(value)) {
      return "double";
    }
    if (yyjson_is_arr(value)) {
      return "array";
    }
    if (yyjson_is_obj(value)) {
      return "object";
    }
    return "null";
  }
  static std::optional<Error> Validate(yyjson_val* item) {
    if (!yyjson_is_obj(item)) {
      return Error{"type_mismatch", String{}, "JsonItem", Type(item), item};
    }
    for (const auto key : {std::string_view("value"), std::string_view("text")}) {
      if (GetJsonField(item, key) == nullptr) {
        return Error{
            "missing_field",
            String(".") + String(key),
            key == "value" ? "int" : "string",
            "missing",
            nullptr,
        };
      }
    }
    auto iterator = yyjson_obj_iter_with(item);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
      const auto name = GetJsonString(key);
      if (name != "value" && name != "text") {
        return Error{
            "unknown_field",
            String(".") + String(name),
            "",
            Type(yyjson_obj_iter_get_val(key)),
            key,
        };
      }
    }
    const auto* number = GetJsonField(item, "value");
    if (!yyjson_is_int(number)) {
      return Error{"type_mismatch", String(".value"), "int", Type(number), number};
    }
    const auto* text = GetJsonField(item, "text");
    if (!yyjson_is_str(text)) {
      return Error{"type_mismatch", String(".text"), "string", Type(text), text};
    }
    return std::nullopt;
  }
  std::optional<Error> ValidateBatch(yyjson_val* root, const String& path) const {
    if (problem_.kind == Kind::JsonObject) {
      auto error = Validate(root);
      if (error) {
        error->path = path + error->path;
      }
      return error;
    }
    auto iterator = yyjson_arr_iter_with(root);
    std::size_t index = 0;
    while (auto* item = yyjson_arr_iter_next(&iterator)) {
      auto error = Validate(item);
      if (error) {
        error->path = path + "[" + String(std::to_string(index)) + "]" + error->path;
        return error;
      }
      ++index;
    }
    return std::nullopt;
  }

  bool IsExpectedError(const Error& error, JsonDocument& document) const {
    const auto& contract = problem_.json;
    const auto offset =
        error.node == nullptr
            ? -1
            : static_cast<std::int64_t>(Offset(contract.input, document, error.node));
    return error.code == contract.code && error.path == contract.path &&
           error.expected == contract.expected && error.actual == contract.actual &&
           offset == contract.offset;
  }

  // 仅错误路径恢复字节位置；DOM 节点与源 token（包含对象键）顺序相同。
  static std::size_t Offset(std::string_view source,
                            JsonDocument& document,
                            const yyjson_val* node) {
    const auto ordinal = static_cast<std::size_t>(node - document.root());
    std::size_t cursor = 0;
    for (std::size_t index = 0; index <= ordinal; ++index) {
      cursor = source.find_first_not_of(" \t\r\n,:]}", cursor);
      if (index == ordinal) {
        return cursor;
      }
      const auto first = source[cursor++];
      if (first == '"') {
        while (source[cursor] != '"') {
          if (source[cursor++] == '\\') {
            ++cursor;
          }
        }
        ++cursor;
      } else if (first != '{' && first != '[') {
        cursor = source.find_first_of(" \t\r\n,]}", cursor);
      }
    }
    std::unreachable();
  }
  Case problem_;
};

void RunJson(benchmark::State& state, const Case& problem, const Options& options) {
  if (options.measurement == Measurement::Allocations) {
    AllocationLedger ledger;
    const AllocationScope scope(ledger);
    JsonReference<true> reference(problem);
    RunReference<Measurement::Allocations>(state, problem, reference, options);
  } else {
    JsonReference<false> reference(problem);
    if (options.measurement == Measurement::Latency) {
      RunReference<Measurement::Latency>(state, problem, reference, options);
    } else {
      RunReference<Measurement::Throughput>(state, problem, reference, options);
    }
  }
}

}  // namespace vela::benchmarks::production
