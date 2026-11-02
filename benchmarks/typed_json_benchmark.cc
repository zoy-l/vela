#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>
#include <yyjson.h>

#include "base/json.h"
#include "benchmarks/process_memory.h"
#include "benchmarks/standard_value_benchmark.h"

namespace vela::benchmarks {
namespace {

enum class DecodeWorkload : std::uint8_t {
  Scalar,
  Object,
  Array,
  NestedGeneric,
  EarlyFailure,
  LateFailure,
  Constructor,
};

enum class DecodeImplementation : std::uint8_t { Typed, Handwritten };
constexpr std::size_t kLargeArrayLength = 1024;
constexpr std::int64_t kExpected = 42;

std::expected<std::string, JsonError> Input(DecodeWorkload workload) {
  JsonOutput output;
  auto* document = output.document();
  const auto item = [&](std::size_t index) {
    auto* object = yyjson_mut_obj(document);
    const bool invalid =
        (workload == DecodeWorkload::EarlyFailure && index == 0) ||
        (workload == DecodeWorkload::LateFailure && index + 1 == kLargeArrayLength);
    AddJsonField(document, object, "value",
                 invalid ? yyjson_mut_bool(document, false) : yyjson_mut_sint(document, kExpected));
    AddJsonField(document, object, "text", yyjson_mut_str(document, "Vela"));
    return object;
  };
  yyjson_mut_val* root = nullptr;
  if (workload == DecodeWorkload::Scalar) {
    root = yyjson_mut_sint(document, kExpected);
  } else if (workload == DecodeWorkload::Object) {
    root = item(0);
  } else {
    root = yyjson_mut_arr(document);
    for (std::size_t index = 0; index < kLargeArrayLength; ++index) {
      yyjson_mut_arr_add_val(root, item(index));
    }
    if (workload == DecodeWorkload::NestedGeneric) {
      for (std::size_t depth = 0; depth < 2; ++depth) {
        auto* box = yyjson_mut_obj(document);
        AddJsonField(document, box, "value", root);
        root = box;
      }
    }
  }
  yyjson_mut_doc_set_root(document, root);
  return output.Serialize();
}

constexpr std::string_view kHandwritten = R"(
class HandError {
  code: string; path: string; expected: string?; actual: string;
  offset: int? = null; message: string = 'JSON input does not match the target';
}
class HandResult<T> { success: bool; value: T? = null; error: HandError? = null; }
func keyPath(path: string, key: string): string {
  let identifier = key.length != 0;
  for (let index = 0; index < key.length; index++) {
    const allowed = index == 0 ? 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_' :
        'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789';
    if (!allowed.includes(key[index])) { identifier = false; break; }
  }
  return identifier ? path + '.' + key : path + '[' + JSON.stringify(key) + ']';
}
class OffsetFrame {
  path: string; object: bool; key: string? = null; index: int = 0;
  nextPath(): string {
    if (!this.object) { return '${this.path}[${this.index++}]'; }
    const key = this.key;
    this.key = null;
    if (key == null) { throw Error(message: 'JSON key was not recorded'); }
    return keyPath(this.path, key);
  }
}
// 语法由普通 JSON.parse 检查；失败后按原始 token 遍历恢复字节偏移。
func tokenOffset(source: string, target: string, keyToken: bool): int? {
  const stack: OffsetFrame[] = [];
  let offset: int? = null;
  let index = 0;
  while (index < source.length) {
    const ch = source[index];
    if (' \t\r\n,:'.includes(ch)) { index++; continue; }
    if (ch == '}' || ch == ']') { stack.pop(); index++; continue; }
    const start = index;
    if (ch == '"') {
      index++;
      while (source[index] != '"') {
        if (source[index] == '\\') { index++; }
        index++;
      }
      index++;
      if (stack.length != 0) {
        const frame = stack[stack.length - 1];
        if (frame.object && frame.key == null) {
          const key = JSON.parse(source.slice(start, index));
          if (!(key is string)) { throw Error(message: 'invalid JSON key'); }
          frame.key = key;
          if (keyToken && offset == null && keyPath(frame.path, key) == target) {
            offset = source.slice(0, start).byteLength;
          }
          continue;
        }
      }
    } else if (ch != '{' && ch != '[') {
      while (index < source.length && !' \t\r\n,]}'.includes(source[index])) { index++; }
    } else { index++; }
    const path = stack.length == 0 ? '$' : stack[stack.length - 1].nextPath();
    if (!keyToken && path == target) { offset = source.slice(0, start).byteLength; }
    if (ch == '{' || ch == '[') { stack.push(OffsetFrame(path: path, object: ch == '{')); }
  }
  return offset;
}
func actualType(value: dynamic): string {
  if (value == null) { return 'null'; }
  if (value is bool) { return 'bool'; }
  if (value is int) { return 'int'; }
  if (value is double) { return 'double'; }
  if (value is string) { return 'string'; }
  return (value is dynamic[]) ? 'array' : 'object';
}
func mismatch(value: dynamic, expected: string, path: string): HandError {
  return HandError(code: 'type_mismatch', path: path, expected: expected, actual: actualType(value));
}
func missing(name: string, expected: string, path: string): HandError {
  return HandError(code: 'missing_field', path: keyPath(path, name), expected: expected, actual: 'missing');
}
func unknown(value: dynamic, name: string, path: string): HandError {
  return HandError(code: 'unknown_field', path: keyPath(path, name), expected: null, actual: actualType(value));
}
func rejected<T>(source: string, error: HandError): HandResult<T> {
  if (error.actual != 'missing') {
    error.offset = tokenOffset(source, error.path, error.code == 'unknown_field');
  }
  return HandResult<T>(success: false, error: error);
}
func itemFields(value: dynamic, numbers: int[], texts: string[], path: string): HandError? {
  if (!(value is Map<string, dynamic>)) { return mismatch(value, 'Item', path); }
  if (!value.has('value')) { return missing('value', 'int', path); }
  if (!value.has('text')) { return missing('text', 'string', path); }
  for (const key in value.keys()) {
    if (key != 'value' && key != 'text') { return unknown(value.get(key), key, path); }
  }
  const number = value.get('value');
  const text = value.get('text');
  if (!(number is int)) { return mismatch(number, 'int', '${path}.value'); }
  if (!(text is string)) { return mismatch(text, 'string', '${path}.text'); }
  numbers.push(number);
  texts.push(text);
  return null;
}
func arrayFields(value: dynamic, numbers: int[], texts: string[], path: string): HandError? {
  if (!(value is dynamic[])) { return mismatch(value, 'Item[]', path); }
  let index = 0;
  for (const item in value) {
    const error = itemFields(item, numbers, texts, '${path}[${index++}]');
    if (error != null) { return error; }
  }
  return null;
}
func materialize(numbers: int[], texts: string[]): Item[] {
  const items: Item[] = [];
  for (let index = 0; index < numbers.length; index++) {
    items.push(Item(value: numbers[index], text: texts[index]));
  }
  return items;
}
func boxFields(value: dynamic, expected: string, element: string, path: string): HandError? {
  if (!(value is Map<string, dynamic>)) { return mismatch(value, expected, path); }
  if (!value.has('value')) { return missing('value', element, path); }
  for (const key in value.keys()) {
    if (key != 'value') { return unknown(value.get(key), key, path); }
  }
  return null;
}
)";

std::string_view TargetType(DecodeWorkload workload) {
  switch (workload) {
    case DecodeWorkload::Scalar:
      return "int";
    case DecodeWorkload::Object:
      return "Item";
    case DecodeWorkload::NestedGeneric:
      return "Box<Box<Item[]>>";
    default:
      return "Item[]";
  }
}

std::string HandwrittenSource(DecodeWorkload workload, const std::string& type) {
  const bool scalar = workload == DecodeWorkload::Scalar;
  const bool object = workload == DecodeWorkload::Object;
  const bool nested = workload == DecodeWorkload::NestedGeneric;
  std::string source(kHandwritten);
  source +=
      "func decode(source: string): HandResult<" + type + "> { const root = JSON.parse(source); ";
  const auto reject = "return rejected<" + type + ">(source, error); ";
  if (scalar) {
    source += "if (!(root is int)) { const error = mismatch(root, 'int', '$'); " + reject + "} ";
    source += "return HandResult<int>(success: true, value: root); } ";
  } else {
    source += "const numbers: int[] = []; const texts: string[] = []; ";
    std::string value = "root";
    std::string path = "$";
    if (nested) {
      source +=
          "const outerError = boxFields(root, 'Box<Box<Item[]>>', 'Box<Item[]>', '$'); "
          "if (outerError != null) { const error = outerError; " +
          reject + "} const inner = root.get('value'); ";
      source +=
          "const innerError = boxFields(inner, 'Box<Item[]>', 'Item[]', '$.value'); "
          "if (innerError != null) { const error = innerError; " +
          reject + "} const items = inner.get('value'); ";
      value = "items";
      path = "$.value.value";
    }
    source += "const error = " + std::string(object ? "itemFields" : "arrayFields") + '(' + value +
              ", numbers, texts, '" + path + "'); if (error != null) { " + reject + "} ";
    std::string result =
        object ? "Item(value: numbers[0], text: texts[0])" : "materialize(numbers, texts)";
    if (nested) {
      result = "Box<Box<Item[]>>(value: Box<Item[]>(value: " + result + "))";
    }
    source += "return HandResult<" + type + ">(success: true, value: " + result + "); } ";
  }
  return source;
}

std::string Source(DecodeWorkload workload,
                   DecodeImplementation implementation,
                   std::string_view input) {
  const bool scalar = workload == DecodeWorkload::Scalar;
  const bool object = workload == DecodeWorkload::Object;
  const bool nested = workload == DecodeWorkload::NestedGeneric;
  const bool failure =
      workload == DecodeWorkload::EarlyFailure || workload == DecodeWorkload::LateFailure;
  const std::string type(TargetType(workload));
  std::string source = "let calls = 0; class Item { value: int; text: string; ";
  if (workload == DecodeWorkload::Constructor) {
    source += "constructor { calls += 1; this.value += this.text.length; } ";
  }
  source += "} class Box<T> { value: T; } ";
  if (implementation == DecodeImplementation::Handwritten) {
    source += HandwrittenSource(workload, type);
  }
  source += "func main(): int { calls = 0; const result = ";
  source +=
      implementation == DecodeImplementation::Typed ? "JSON.safeParse<" + type + ">" : "decode";
  source += "('" + std::string(input) + "'); ";
  if (failure) {
    source += "if (result.success || calls != 0) { return 0; } const error = result.error; ";
    if (implementation == DecodeImplementation::Handwritten) {
      source += "if (error == null) { return 0; } ";
    }
    const auto index = workload == DecodeWorkload::EarlyFailure ? 0 : kLargeArrayLength - 1;
    const auto offset = input.find("false");
    return source + "return error.code == 'type_mismatch' && error.path == '$[" +
           std::to_string(index) +
           "].value' && error.expected == 'int' && error.actual == 'bool' "
           "&& error.offset == " +
           std::to_string(offset) + " ? 42 : 0; }";
  }
  source += "if (!result.success) { return 0; } const value = result.value; ";
  if (implementation == DecodeImplementation::Handwritten) {
    source += "if (value == null) { return 0; } ";
  }
  if (scalar) {
    return source + "return value == 42 ? 42 : 0; }";
  }
  if (object) {
    return source + "return value.value == 42 && value.text == 'Vela' ? 42 : 0; }";
  }
  const std::string items = nested ? "value.value.value" : "value";
  source += "return " + items + ".length == " + std::to_string(kLargeArrayLength) + " && " + items +
            '[' + std::to_string(kLargeArrayLength - 1) + "].value == ";
  source += workload == DecodeWorkload::Constructor ? "46 && calls == 1024" : "42";
  return source + " ? 42 : 0; }";
}

void Decode(benchmark::State& state, DecodeWorkload workload, DecodeImplementation implementation) {
  const auto input = Input(workload);
  if (!input) {
    state.SkipWithError(input.error().message);
    return;
  }
  SteadyValue(state, Source(workload, implementation, *input), kExpected);
  const auto peak = PeakResidentBytes();
  if (!peak) {
    state.SkipWithError(peak.error());
    return;
  }
  state.counters["process_peak_resident_bytes"] = static_cast<double>(*peak);
  state.counters["input_bytes"] = static_cast<double>(input->size());
}

BENCHMARK_CAPTURE(Decode, TypedScalar, DecodeWorkload::Scalar, DecodeImplementation::Typed)
    ->Name("typed_json/typed/scalar");
BENCHMARK_CAPTURE(Decode, ManualScalar, DecodeWorkload::Scalar, DecodeImplementation::Handwritten)
    ->Name("typed_json/handwritten/scalar");
BENCHMARK_CAPTURE(Decode, TypedObject, DecodeWorkload::Object, DecodeImplementation::Typed)
    ->Name("typed_json/typed/object");
BENCHMARK_CAPTURE(Decode, ManualObject, DecodeWorkload::Object, DecodeImplementation::Handwritten)
    ->Name("typed_json/handwritten/object");
BENCHMARK_CAPTURE(Decode, TypedArray, DecodeWorkload::Array, DecodeImplementation::Typed)
    ->Name("typed_json/typed/large_array");
BENCHMARK_CAPTURE(Decode, ManualArray, DecodeWorkload::Array, DecodeImplementation::Handwritten)
    ->Name("typed_json/handwritten/large_array");
BENCHMARK_CAPTURE(Decode, TypedNested, DecodeWorkload::NestedGeneric, DecodeImplementation::Typed)
    ->Name("typed_json/typed/nested_generic");
BENCHMARK_CAPTURE(Decode,
                  ManualNested,
                  DecodeWorkload::NestedGeneric,
                  DecodeImplementation::Handwritten)
    ->Name("typed_json/handwritten/nested_generic");
BENCHMARK_CAPTURE(Decode, TypedEarly, DecodeWorkload::EarlyFailure, DecodeImplementation::Typed)
    ->Name("typed_json/typed/early_failure");
BENCHMARK_CAPTURE(Decode,
                  ManualEarly,
                  DecodeWorkload::EarlyFailure,
                  DecodeImplementation::Handwritten)
    ->Name("typed_json/handwritten/early_failure");
BENCHMARK_CAPTURE(Decode, TypedLate, DecodeWorkload::LateFailure, DecodeImplementation::Typed)
    ->Name("typed_json/typed/late_failure");
BENCHMARK_CAPTURE(Decode,
                  ManualLate,
                  DecodeWorkload::LateFailure,
                  DecodeImplementation::Handwritten)
    ->Name("typed_json/handwritten/late_failure");
BENCHMARK_CAPTURE(Decode,
                  TypedConstructor,
                  DecodeWorkload::Constructor,
                  DecodeImplementation::Typed)
    ->Name("typed_json/typed/constructor");
BENCHMARK_CAPTURE(Decode,
                  ManualConstructor,
                  DecodeWorkload::Constructor,
                  DecodeImplementation::Handwritten)
    ->Name("typed_json/handwritten/constructor");

}  // namespace
}  // namespace vela::benchmarks
