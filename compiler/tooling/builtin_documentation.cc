#include "compiler/tooling/builtin_documentation.h"

#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "compiler/frontend/builtins.h"
#include "compiler/frontend/type_checker.h"

namespace vela::tooling {
namespace {

using Kind = frontend::TypeDescriptor::Kind;

std::string_view RegExpDocumentation(Kind receiver, std::string_view name) {
  if (receiver == Kind::RegExp || name == "RegExp") {
    if (name == "allMatches") {
      return "Returns independent, non-overlapping matches in code-point order. start defaults "
             "to zero. Empty matches advance one code point and include the terminal empty once.";
    }
    if (name == "matchAsPrefix") {
      return "Attempts an anchored match at the code-point start (default zero) in the whole "
             "input. Lookbehind retains preceding context; \\K may reset the reported start.";
    }
    if (name == "isUnicode") {
      return "Always true. UTF and Unicode properties are enabled; \\C is rejected.";
    }
    return "Immutable PCRE2 Unicode regular expression. Defaults: multiLine false, caseSensitive "
           "true, dotAll false. Uses code-point coordinates and no shared matching cursor. "
           "Compilation and matching failures throw typed Error codes.";
  }
  if (receiver == Kind::RegExpMatch || name == "RegExpMatch") {
    if (name == "groupNames") {
      return "Independent unique-name snapshot in native name-table order.";
    }
    if (name == "namedGroup") {
      return "Returns the first participating capture for this name, or null if none participate. "
             "An unknown name throws RegExpGroupError.";
    }
    return "Immutable match retaining its input and pattern. start/end are Unicode code-point "
           "offsets; group zero is the whole match. Unmatched captures return null; empty "
           "participating captures return an empty string. Invalid group indices throw "
           "IndexOutOfBounds.";
  }
  return {};
}

std::string_view ByteDocumentation(Kind receiver, std::string_view name) {
  if (receiver == Kind::ByteArray) {
    if (name == "length") {
      return "Read-only fixed byte length. ByteArray stores checked int bytes in 0..255.";
    }
    if (name == "slice") {
      return "Copies a normalized half-open byte range into independent fixed storage. Negative "
             "endpoints count from the end; endpoints clamp to the byte length.";
    }
  }
  if (receiver == Kind::ByteData) {
    if (name == "byteLength") {
      return "Read-only byte length of this shared, owner-retaining view.";
    }
    if (frontend::FindBuiltin(receiver, name)) {
      return "Checks a view-relative byte offset and performs unaligned typed byte access. "
             "Multi-byte access defaults to Endian.big. Integer writes reject overflow before "
             "modifying storage; floating writes preserve IEEE values.";
    }
  }
  return {};
}

std::string_view AsyncDocumentation(Kind receiver, std::string_view name) {
  if (receiver == Kind::Future) {
    if (name == "then") {
      return "Queues a typed continuation. Values and returned Futures are flattened; thrown "
             "callback errors fail the result.";
    }
    if (name == "catchError") {
      return "Handles errors matching the callback's non-null class or interface parameter. "
             "Unmatched errors propagate unchanged.";
    }
    if (name == "whenComplete") {
      return "Runs action after success or failure, waits for its Future, and preserves the source "
             "result unless action fails.";
    }
    if (name == "timeout") {
      return "Uses the source result before the deadline, otherwise onTimeout or TimeoutException. "
             "The source remains observed.";
    }
    if (name == "wait") {
      return "Collects values in input order. eagerError controls early failure; cleanUp releases "
             "non-null successes after any input fails.";
    }
    if (name == "race") {
      return "Adopts the first completion and observes every input. Empty inputs remain pending.";
    }
    return "Creates a typed Future. Computations may return values or Futures; microtask queues "
           "work before timers.";
  }
  if (receiver == Kind::Completer) {
    if (name == "future") {
      return "The Future completed by this producer.";
    }
    if (name == "isCompleted") {
      return "Whether a completion has been requested, including adoption of a pending Future.";
    }
    return "Completes once with a value, Future, or throwable object. Duplicate completion and "
           "self-adoption throw catchable Errors.";
  }
  if (receiver == Kind::Timer) {
    return "Schedules callbacks on the Isolate thread. cancel() releases the callback and prevents "
           "future ticks.";
  }
  if (receiver == Kind::CancellationToken) {
    return "A separate cancellation request shared with native operations. cancel() is idempotent; "
           "the operation still completes its Future.";
  }
  if (name == "Future" || name == "Completer" || name == "Timer" || name == "CancellationToken") {
    return "Typed asynchronous operations. See the async/Future language contract.";
  }
  if (name == "sleep") {
    return "Returns Future<void> after a nonnegative monotonic delay in integer milliseconds.";
  }
  return {};
}

}  // namespace

std::string BuiltinDetail(const frontend::FunctionDescriptor& signature) {
  std::string text = "(";
  for (const auto& parameter : signature.parameters) {
    if (text.back() != '(') {
      text += ", ";
    }
    text += parameter.rest ? "..." : "";
    text += parameter.name;
    if (signature.optional_parameters.contains(parameter.name)) {
      text += '?';
    }
    const auto type_name =
        parameter.rest ? frontend::MakeArrayType(parameter.type).name : parameter.type.name;
    text += ": " + type_name;
  }
  return text + "): " + signature.return_type.name;
}

std::string_view BuiltinDocumentation(Kind receiver, std::string_view name) {
  if (receiver == Kind::Invalid && name == "dynamic") {
    return "Retains the actual language type, including null. Assignments to concrete types and "
           "member, call, index, and operator use are checked at runtime. Containers retain "
           "their declared element types.";
  }
  if (const auto regexp = RegExpDocumentation(receiver, name); !regexp.empty()) {
    return regexp;
  }
  if (const auto bytes = ByteDocumentation(receiver, name); !bytes.empty()) {
    return bytes;
  }
  if (const auto async = AsyncDocumentation(receiver, name); !async.empty()) {
    return async;
  }
  static const absl::flat_hash_map<std::pair<Kind, std::string_view>, std::string_view>
      kDocumentation = {
          {
              {Kind::String, "toLowerCase"},
              "Returns Unicode default lowercase, including full and contextual mappings. "
              "Independent of locale; leaves the receiver unchanged and performs no normalization.",
          },
          {
              {Kind::String, "toUpperCase"},
              "Returns Unicode default uppercase, including full mappings. "
              "Independent of locale; leaves the receiver unchanged and performs no normalization.",
          },
          {
              {Kind::Invalid, "print"},
              "Writes a value followed by a newline. Returns void.\n\n"
              "Strings, int, double, bool and null use the same formatting as string "
              "interpolation. "
              "Other references, including class instances and arrays, print as object without "
              "invoking user toString() methods.\n\n"
              "```vela\nprint('Hello, ${42}!');\n```",
          },
          {
              {Kind::Invalid, "ok"},
              "Creates the success alternative of Expected<T, E>. T and E come from the expected "
              "variable, return or parameter type.\n\n"
              "value must match T. Omit the argument when T is void. Creating a result does not "
              "throw it.",
          },
          {
              {Kind::Invalid, "err"},
              "Creates the error alternative of Expected<T, E>. T and E come from the expected "
              "variable, return or parameter type.\n\n"
              "error must match E. Omit the argument when E is void. Creating an error result does "
              "not automatically throw.",
          },
          {
              {Kind::Invalid, "String"},
              "A mutable String, initially empty. Repeated append() calls have amortized "
              "linear cost.\n\n"
              "toString() returns an independent, immutable string snapshot.",
          },
          {
              {Kind::Invalid, "Set"},
              "An insertion-ordered collection of unique values of type T. Lookup, insertion and "
              "deletion take average O(1) time.\n\n"
              "Strings compare by content; class instances compare by identity. size returns the "
              "entry count.",
          },
          {
              {Kind::Invalid, "Map"},
              "An insertion-ordered mapping from keys K to values V. Lookup, insertion and "
              "deletion take average O(1) time.\n\n"
              "get(key) returns null for a missing key. Updating an existing key preserves its "
              "position.",
          },
          {
              {Kind::Invalid, "Expected"},
              "Represents either a success value T or an error value E. Construct results with "
              "ok(value) or err(error), then inspect hasValue().\n\n"
              "Reading the inactive alternative produces a runtime error.",
          },
          {
              {Kind::Invalid, "int"},
              "A signed 64-bit integer. Arithmetic and assignment do not implicitly convert "
              "between int and double. Use toDouble() explicitly.",
          },
          {
              {Kind::Invalid, "double"},
              "An IEEE 754 double-precision floating-point value, including NaN and positive or "
              "negative Infinity. Use toInt() for explicit integer conversion.",
          },
          {
              {Kind::Invalid, "bool"},
              "A boolean value: true or false. Conditions require bool; other values are not "
              "implicitly converted by truthiness.",
          },
          {
              {Kind::Invalid, "string"},
              "An immutable, valid UTF-8 string. length and slice() use Unicode code points; "
              "byteLength counts bytes.\n\nThere is no separate char type.",
          },
          {
              {Kind::Invalid, "void"},
              "Indicates that a function returns no value. A void result cannot be stored in a "
              "variable or used as an ordinary value.",
          },
          {
              {Kind::String, "length"},
              "Returns the number of Unicode code points as int, not the byte count or perceived "
              "character count.\n\n"
              "Read-only. O(UTF-8 byte length). A combining character sequence may contain "
              "multiple code points.",
          },
          {
              {Kind::String, "byteLength"},
              "Returns the UTF-8 byte count as int, including embedded NUL bytes.\n\nRead-only. "
              "O(1).",
          },
          {
              {Kind::String, "graphemeLength"},
              "Returns the number of extended grapheme clusters as int, approximating "
              "user-perceived characters.\n\nRead-only. Scans the string.",
          },
          {
              {Kind::String, "slice"},
              "Returns a new string containing the half-open range [start, end).\n\n"
              "- start: inclusive Unicode code point index.\n- end: exclusive Unicode code point "
              "index.\n\n"
              "Requires 0 <= start <= end <= length. Negative indices and automatic bounds "
              "clamping are not supported.\n\n"
              "```vela\n'hello'.slice(1, 4); // 'ell'\n```",
          },
          {
              {Kind::String, "indexOf"},
              "Returns the Unicode code point index of the first occurrence of text, or -1 if "
              "absent. An empty text matches at 0.\n\n"
              "Matches exact UTF-8 content without Unicode normalization.",
          },
          {
              {Kind::String, "includes"},
              "Returns whether the string contains text. An empty text returns true.\n\nMatches "
              "exact content and is case-sensitive.",
          },
          {
              {Kind::String, "startsWith"},
              "Returns whether the string starts with prefix. An empty prefix returns true.",
          },
          {
              {Kind::String, "endsWith"},
              "Returns whether the string ends with suffix. An empty suffix returns true.",
          },
          {
              {Kind::String, "codePoints"},
              "Returns a new int[] of Unicode code points in order.\n\nOne linear scan produces an "
              "array with O(1) indexed access, suitable for repeated traversal.",
          },
          {
              {Kind::String, "graphemes"},
              "Returns a new string[] split into Unicode extended grapheme clusters. Combining "
              "marks and emoji sequences may belong to one element.",
          },
          {
              {Kind::String, "graphemeSlice"},
              "Returns a new string for the half-open range [start, end), measured in extended "
              "grapheme clusters.\n\n"
              "Requires 0 <= start <= end <= graphemeLength.",
          },
          {
              {Kind::MutableString, "append"},
              "Copies text to the end of the builder. Returns void.\n\nGeometric buffer growth "
              "gives repeated appends amortized linear cost. Existing snapshots remain unchanged.",
          },
          {
              {Kind::MutableString, "toString"},
              "Returns an independent, immutable string snapshot of the current content.\n\nLater "
              "append() calls do not change the returned string.",
          },
          {
              {Kind::Int, "toString"},
              "Returns a decimal string using the same locale-independent formatting as print() "
              "and string interpolation.",
          },
          {
              {Kind::Double, "toString"},
              "Returns the shortest round-trip string using the same formatting as print(). "
              "Preserves negative zero and supports NaN and Infinity.",
          },
          {
              {Kind::Bool, "toString"},
              "Returns the string 'true' or 'false'.",
          },
          {
              {Kind::Int, "toInt"},
              "Returns the current int value unchanged.",
          },
          {
              {Kind::Int, "toDouble"},
              "Explicitly converts to double. Large integers may lose precision beyond the exact "
              "integer range of double.",
          },
          {
              {Kind::Double, "toDouble"},
              "Returns the current double value unchanged.",
          },
          {
              {Kind::Double, "toInt"},
              "Truncates toward zero and returns int. NaN, Infinity and values outside the int "
              "range produce a runtime error.\n\n"
              "```vela\n(-3.9).toInt(); // -3\n```",
          },
          {
              {Kind::Array, "length"},
              "Returns the current number of elements as int in O(1) time. Read-only. Use push(), "
              "pop(), shift(), unshift(), splice() or clear() to change the length.",
          },
          {
              {Kind::Array, "capacity"},
              "Returns the number of elements that fit without reallocating storage. Read-only; "
              "always at least length. reserve() may increase it; removals retain it.",
          },
          {
              {Kind::Array, "push"},
              "Appends zero or more items of the array's element type and returns the new length. "
              "Amortized O(1) per item; growth preserves the array's identity and all aliases. "
              "No implicit numeric conversion or sparse holes.",
          },
          {
              {Kind::Array, "pop"},
              "Removes and returns the last element, or null if empty. Returns T? (without nested "
              "nullable types). O(1); capacity is retained.",
          },
          {
              {Kind::Array, "unshift"},
              "Prepends zero or more items in argument order and returns the new length. "
              "Moves the suffix once in O(n + item count) time.",
          },
          {
              {Kind::Array, "shift"},
              "Removes and returns the first element, or null when empty. O(n) time; retains "
              "capacity.",
          },
          {
              {Kind::Array, "splice"},
              "Removes deleteCount elements starting at start, inserts items in their place and "
              "returns the removed elements as an independent T[]. Negative start counts from the "
              "end; bounds are clamped. Omitted deleteCount removes through the end; splice() "
              "changes nothing. O(n + item count) time.",
          },
          {
              {Kind::Array, "clear"},
              "Removes all elements and returns void in O(1) time. Retains capacity for reuse. "
              "Removed references no longer keep objects alive during GC.",
          },
          {
              {Kind::Array, "reserve"},
              "Ensures capacity for at least capacity elements without changing length. "
              "Negative capacities throw Error. O(n) when storage must grow, O(1) otherwise. "
              "Does not initialize new elements or create holes.",
          },
          {
              {Kind::Array, "join"},
              "Joins elements with separator and returns string. An empty array returns an empty "
              "string. The separator defaults to a comma.\n\n"
              "Supports string, int, double, bool and nullable elements; null contributes empty "
              "text. "
              "Calculates total length before allocating the final string once.",
          },
          {
              {Kind::Array, "slice"},
              "Returns a shallow copy of the half-open element range [start, end) as a new T[].\n\n"
              "Start defaults to zero and end to length. Negative indices count from the end; "
              "bounds are clamped. A reversed range produces an empty array. slice() copies all "
              "elements.",
          },
          {
              {Kind::Array, "fill"},
              "Fills [start, end) in place and returns this array. Start defaults to zero and end "
              "to length; negative indices count from the end and bounds are clamped. "
              "value must match the element type. Length is unchanged; O(n) time.",
          },
          {
              {Kind::Array, "reverse"},
              "Reverses elements in place and returns this array. O(n) time without allocating "
              "another "
              "array.",
          },
          {
              {Kind::Array, "sort"},
              "Sorts in place and returns this array. Sorting is stable.\n\n"
              "compareFn(left, right) returns a negative, zero or positive int/double; NaN means "
              "equal. "
              "Without a comparator, primitive elements sort by their UTF-16 string "
              "representations. "
              "Worst-case O(n log n) comparisons and O(n) auxiliary array space. Comparisons read "
              "a "
              "snapshot. Structurally modifying the receiver from compareFn throws Error.\n\n"
              "```vela\nvalues.sort((left, right) => left - right);\n```",
          },
          {
              {Kind::Array, "forEach"},
              "Calls callbackFn(value, index, array) in index order and returns void. Unused "
              "trailing "
              "parameters may be omitted; callback results are ignored. Structural changes to this "
              "array from "
              "the callback throw Error.",
          },
          {
              {Kind::Array, "some"},
              "Returns true if any element satisfies predicate. Stops at the first true result. "
              "Returns false for an empty array.",
          },
          {
              {Kind::Array, "every"},
              "Returns true only if all elements satisfy predicate. Stops at the first false "
              "result. Returns true for an empty array.",
          },
          {
              {Kind::Array, "findIndex"},
              "Returns the index of the first element satisfying predicate, or -1 if no element "
              "matches.",
          },
          {
              {Kind::Array, "reduce"},
              "Calls callbackFn(accumulator, value, index, array) in index order. Unused trailing "
              "parameters may be omitted. initialValue is required and determines R. Traversal "
              "starts at index zero; an empty array returns initialValue without calling "
              "callbackFn.\n\n"
              "```vela\nvalues.reduce((sum, value) => sum + value, 0);\n```",
          },
          {
              {Kind::Array, "map"},
              "Transforms elements in index order into a new R[] with independent storage. Leaves "
              "the original array unchanged.\n\n"
              "callbackFn(value, index, array) may omit unused trailing parameters. R is inferred "
              "from the "
              "callback result. Allocates the result array once. References are not "
              "deep-copied. Structural changes to the source from the callback throw Error.\n\n"
              "```vela\nvalues.map(value => value.toString());\n```",
          },
          {
              {Kind::Set, "size"},
              "Returns the number of distinct entries as int in O(1) time. Read-only.",
          },
          {
              {Kind::Set, "add"},
              "Adds value. Returns true for a newly inserted value, or false if already present. "
              "New entries appear last. Average O(1) time.",
          },
          {
              {Kind::Set, "has"},
              "Returns whether value is present. Average O(1) time.",
          },
          {
              {Kind::Set, "delete"},
              "Deletes value. Returns true if an entry was removed, or false if absent. Average "
              "O(1) time.",
          },
          {
              {Kind::Set, "clear"},
              "Removes all entries and returns void.",
          },
          {
              {Kind::Set, "values"},
              "Returns an independent T[] snapshot in insertion order. Later set mutations do not "
              "change the snapshot.",
          },
          {
              {Kind::Set, "union"},
              "Returns a new union with other without changing either set. Keeps this set's "
              "insertion order, then appends unseen entries from other.",
          },
          {
              {Kind::Set, "intersection"},
              "Returns a new intersection with other without changing either set. Results follow "
              "this set's insertion order.",
          },
          {
              {Kind::Set, "difference"},
              "Returns a new set containing entries absent from other. Changes neither set and "
              "preserves this set's insertion order.",
          },
          {
              {Kind::Map, "size"},
              "Returns the number of key-value entries as int in O(1) time. Read-only.",
          },
          {
              {Kind::Map, "has"},
              "Returns whether key is present. Average O(1) time.",
          },
          {
              {Kind::Map, "delete"},
              "Deletes the entry for key. Returns true if removed, or false if absent. Average "
              "O(1) time.",
          },
          {
              {Kind::Map, "clear"},
              "Removes all key-value entries and returns void.",
          },
          {
              {Kind::Map, "get"},
              "Returns the value V? for key, or null if absent. Average O(1) time.\n\n"
              "If V is nullable, use has(key) to distinguish a missing key from a stored null.",
          },
          {
              {Kind::Map, "set"},
              "Associates key with value and returns void. Average O(1) time. New keys appear "
              "last; updating an existing key preserves its position.",
          },
          {
              {Kind::Map, "keys"},
              "Returns an independent K[] snapshot of keys in insertion order.",
          },
          {
              {Kind::Map, "values"},
              "Returns an independent V[] snapshot of values in key insertion order.",
          },
          {
              {Kind::Expected, "hasValue"},
              "Returns whether the result contains a success value. When true, use value(); when "
              "false, use error().",
          },
          {
              {Kind::Expected, "value"},
              "Returns the success value T. Produces a runtime error if the result contains an "
              "error instead. Check hasValue() first.",
          },
          {
              {Kind::Expected, "error"},
              "Returns the error value E. Produces a runtime error if the result contains a "
              "success value instead. Check hasValue() first.",
          },
      };
  const auto found = kDocumentation.find({receiver, name});
  return found == kDocumentation.end() ? std::string_view{} : found->second;
}

std::string_view BuiltinTypeDetail(std::string_view name) {
  static const absl::flat_hash_map<std::string_view, std::string_view> kTypes = {
      {"int", "int"},
      {"double", "double"},
      {"bool", "bool"},
      {"string", "string"},
      {"void", "void"},
      {"dynamic", "dynamic"},
      {"String", "class String {\n  constructor();\n}"},
      {"ByteArray", "class ByteArray {\n  constructor(length: int);\n}"},
      {"ByteData", "type ByteData"},
      {
          "RegExp",
          "class RegExp {\n  constructor(source: string, multiLine?: bool, caseSensitive?: bool, "
          "dotAll?: bool);\n}",
      },
      {"RegExpMatch", "type RegExpMatch"},
      {"Set", "class Set<T> {\n  constructor();\n}"},
      {"Map", "class Map<K, V> {\n  constructor();\n}"},
      {"Expected", "type Expected<T, E>"},
      {"Future", "type Future<T>"},
      {"Completer", "class Completer<T> {\n  constructor();\n}"},
      {"Timer", "type Timer"},
      {"CancellationToken", "class CancellationToken {\n  constructor();\n}"},
  };
  const auto found = kTypes.find(name);
  return found == kTypes.end() ? std::string_view{} : found->second;
}

}  // namespace vela::tooling
