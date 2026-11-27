#include <cstdint>
#include <string>
#include <string_view>

#include "benchmark/benchmark.h"

#include "benchmarks/standard_value_benchmark.h"

namespace vela::benchmarks {
namespace {

enum class SearchOperation : std::uint8_t { Filter, Find, Includes, NarrowFilter };
enum class SearchImplementation : std::uint8_t { Existing, Builtin };
enum class IncludesStorage : std::uint8_t {
  Objects,
  Structs,
  TaggedIntegers,
  TaggedDoubles,
  TaggedNaNs,
};
constexpr std::int64_t kExpectedAnswer = 42;

std::string SearchSource(SearchOperation operation,
                         SearchImplementation implementation,
                         std::int64_t count) {
  std::string source;
  if (operation != SearchOperation::NarrowFilter) {
    const auto size = std::to_string(count);
    source =
        "const values: int[] = make(); func make(): int[] {"
        "const result: int[] = []; result.reserve(" +
        size + "); for (let i = 0; i < " + size + "; i++) { result.push(i); } return result; }";
  }
  source += "func main(): int {";
  if (operation == SearchOperation::NarrowFilter) {
    source += "const mixed: dynamic[] = [1, 'text', 42, null];";
    source += implementation == SearchImplementation::Builtin
                  ? "const result: int[] = mixed.filter(value => {"
                    "if (value is int) { return value > 0; } return false; });"
                  : "const result: int[] = []; result.reserve(mixed.length);"
                    "mixed.forEach(value => { if (value is int) {"
                    "if (value > 0) { result.push(value); } } });";
    source += "return result.length == 2 && result[1] == 42 ? 42 : 0; }";
  } else if (operation == SearchOperation::Filter) {
    source += implementation == SearchImplementation::Builtin
                  ? "const result = values.filter(value => value % 2 == 0);"
                  : "const result: int[] = []; result.reserve(values.length);"
                    "values.forEach(value => { if (value % 2 == 0) { result.push(value); } });";
    source +=
        "return result.length == values.length / 2 && "
        "result[result.length - 1] == values.length - 2 ? 42 : 0; }";
  } else {
    source +=
        "let total = 0; for (let offset = 1; offset <= 16; offset++) {"
        "const target = values.length - offset;";
    if (operation == SearchOperation::Find) {
      source += implementation == SearchImplementation::Builtin
                    ? "const found = values.find(value => value == target);"
                      "if (found == null || found != target) { return 0; }"
                    : "const index = values.findIndex(value => value == target);"
                      "if (index < 0 || values[index] != target) { return 0; }";
    } else {
      source += implementation == SearchImplementation::Builtin
                    ? "if (!values.includes(target)) { return 0; }"
                    : "if (!values.some(value => value == target)) { return 0; }";
    }
    source += "total++; } return total == 16 ? 42 : 0; }";
  }
  return source;
}

void ArraySearch(benchmark::State& state,
                 SearchOperation operation,
                 SearchImplementation implementation) {
  const auto count = operation == SearchOperation::NarrowFilter ? 0 : state.range(0);
  SteadyValue(state, SearchSource(operation, implementation, count), kExpectedAnswer,
              HeapBaseline::AfterWarmup);
}

void ArrayIncludesStorage(benchmark::State& state, IncludesStorage storage) {
  std::string_view type = "dynamic[]";
  std::string_view element;
  std::string_view needle;
  bool nan = false;
  switch (storage) {
    case IncludesStorage::Objects:
      type = "Item[]";
      element = "Item(value: 1)";
      needle = "Item(value: 42)";
      break;
    case IncludesStorage::Structs:
      type = "Pair[]";
      element = "Pair(first: 1, second: 2)";
      needle = "Pair(first: 1, second: 42)";
      break;
    case IncludesStorage::TaggedIntegers:
      element = "1";
      needle = "42";
      break;
    case IncludesStorage::TaggedDoubles:
      element = "1.0";
      needle = "42.0";
      break;
    case IncludesStorage::TaggedNaNs:
      element = "1.0";
      needle = "Math.pow(-1.0, 0.5)";
      nan = true;
      break;
  }
  std::string source =
      "class Item { value: int; } struct Pair { first: int; second: int; } const values: ";
  source += type;
  source += " = make(); func make(): ";
  source += type;
  source += " { const result: ";
  source += type;
  source +=
      " = []; for (let i = 0; i < " + std::to_string(state.range(0)) + "; i++) { result.push(";
  source += element;
  source += "); }";
  if (nan) {
    source += "result.push(Math.pow(-1.0, 0.5));";
  }
  source += "return result; } const needle = ";
  source += needle;
  source += "; func main(): int { return values.includes(needle) ? ";
  source += nan ? "42 : 0; }" : "0 : 42; }";
  SteadyValue(state, source, kExpectedAnswer, HeapBaseline::AfterWarmup);
}

BENCHMARK_CAPTURE(ArrayIncludesStorage, Objects, IncludesStorage::Objects)
    ->Name("array_search/includes/objects")
    ->Arg(4096)
    ->UseRealTime();
BENCHMARK_CAPTURE(ArrayIncludesStorage, Structs, IncludesStorage::Structs)
    ->Name("array_search/includes/structs")
    ->Arg(4096)
    ->UseRealTime();
BENCHMARK_CAPTURE(ArrayIncludesStorage, TaggedIntegers, IncludesStorage::TaggedIntegers)
    ->Name("array_search/includes/tagged_integers")
    ->Arg(4096)
    ->UseRealTime();
BENCHMARK_CAPTURE(ArrayIncludesStorage, TaggedDoubles, IncludesStorage::TaggedDoubles)
    ->Name("array_search/includes/tagged_doubles")
    ->Arg(4096)
    ->UseRealTime();
BENCHMARK_CAPTURE(ArrayIncludesStorage, TaggedNaNs, IncludesStorage::TaggedNaNs)
    ->Name("array_search/includes/tagged_nans")
    ->Arg(4096)
    ->UseRealTime();

BENCHMARK_CAPTURE(ArraySearch,
                  FilterExisting,
                  SearchOperation::Filter,
                  SearchImplementation::Existing)
    ->Name("array_search/filter/existing")
    ->Arg(64)
    ->Arg(4096)
    ->UseRealTime();
BENCHMARK_CAPTURE(ArraySearch,
                  FilterBuiltin,
                  SearchOperation::Filter,
                  SearchImplementation::Builtin)
    ->Name("array_search/filter/builtin")
    ->Arg(64)
    ->Arg(4096)
    ->UseRealTime();
BENCHMARK_CAPTURE(ArraySearch, FindExisting, SearchOperation::Find, SearchImplementation::Existing)
    ->Name("array_search/find/existing")
    ->Arg(64)
    ->Arg(4096)
    ->UseRealTime();
BENCHMARK_CAPTURE(ArraySearch, FindBuiltin, SearchOperation::Find, SearchImplementation::Builtin)
    ->Name("array_search/find/builtin")
    ->Arg(64)
    ->Arg(4096)
    ->UseRealTime();
BENCHMARK_CAPTURE(ArraySearch,
                  IncludesExisting,
                  SearchOperation::Includes,
                  SearchImplementation::Existing)
    ->Name("array_search/includes/existing")
    ->Arg(64)
    ->Arg(4096)
    ->UseRealTime();
BENCHMARK_CAPTURE(ArraySearch,
                  IncludesBuiltin,
                  SearchOperation::Includes,
                  SearchImplementation::Builtin)
    ->Name("array_search/includes/builtin")
    ->Arg(64)
    ->Arg(4096)
    ->UseRealTime();
BENCHMARK_CAPTURE(ArraySearch,
                  NarrowFilterExisting,
                  SearchOperation::NarrowFilter,
                  SearchImplementation::Existing)
    ->Name("array_search/narrow_filter/existing")
    ->UseRealTime();
BENCHMARK_CAPTURE(ArraySearch,
                  NarrowFilterBuiltin,
                  SearchOperation::NarrowFilter,
                  SearchImplementation::Builtin)
    ->Name("array_search/narrow_filter/builtin")
    ->UseRealTime();

}  // namespace
}  // namespace vela::benchmarks
