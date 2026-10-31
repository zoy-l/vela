#include "runtime/regexp.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <pcre2.h>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/regexp_diagnostic.h"
#include "base/utf8.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/regexp_storage.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

using Result = std::expected<Value, RegExpFailure>;

std::unexpected<RegExpFailure> Failure(ExecutionStatus status, std::string message = {}) {
  if (message.empty()) {
    message = ExecutionStatusName(status);
  }
  return std::unexpected(RegExpFailure{.status = status, .message = std::move(message)});
}

Result Allocated(HeapObject* object) {
  return object != nullptr ? Result(Value::Object(object)) : Failure(ExecutionStatus::OutOfMemory);
}

Result Text(Heap& heap, std::string_view text) {
  auto* result = heap.AllocateString(text);
  return result != nullptr ? Result(Value::String(result)) : Failure(ExecutionStatus::OutOfMemory);
}

void* AllocatePcre(std::size_t size, void* data) {
  return static_cast<Heap*>(data)->AllocateNative(size);
}

void FreePcre(void* pointer, void* data) {
  static_cast<Heap*>(data)->FreeNative(pointer);
}

template <typename T, auto Free>
using PcreOwner = std::unique_ptr<T, decltype(Free)>;

class NativeBlock final {
 public:
  NativeBlock(Heap& heap, std::size_t size) : heap_(heap), memory_(heap.AllocateNative(size)) {}
  ~NativeBlock() { heap_.FreeNative(memory_); }
  NativeBlock(const NativeBlock&) = delete;
  NativeBlock& operator=(const NativeBlock&) = delete;
  NativeBlock(NativeBlock&&) = delete;
  NativeBlock& operator=(NativeBlock&&) = delete;
  [[nodiscard]] void* data() const noexcept { return memory_; }

 private:
  Heap& heap_;
  void* memory_;
};

class Boundaries final {
 public:
  Boundaries(Heap& heap, std::string_view text) : heap_(heap), text_(text) {}
  ~Boundaries() { heap_.FreeNative(points_.data()); }
  Boundaries(const Boundaries&) = delete;
  Boundaries& operator=(const Boundaries&) = delete;
  Boundaries(Boundaries&&) = delete;
  Boundaries& operator=(Boundaries&&) = delete;
  [[nodiscard]] std::expected<void, ExecutionStatus> Initialize() {
    if (!points_.empty()) {
      return {};
    }
    const auto count = Utf8Length(text_);
    auto* storage =
        static_cast<std::size_t*>(heap_.AllocateNative((count + 1) * sizeof(std::size_t)));
    if (storage == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    points_ = {storage, count + 1};
    std::size_t index = 0;
    // 输入已验证 UTF-8，每个非续字节就是一个码点边界；无需再次解码标量。
    for (std::size_t byte = 0; byte < text_.size(); ++byte) {
      if ((static_cast<unsigned char>(text_.substr(byte).front()) & kContinuationMask) !=
          kContinuationTag) {
        points_.subspan(index++).front() = byte;
      }
    }
    points_.subspan(index).front() = text_.size();
    return {};
  }
  [[nodiscard]] std::size_t Point(std::size_t byte) const noexcept {
    return static_cast<std::size_t>(std::ranges::lower_bound(points_, byte) - points_.begin());
  }

 private:
  Heap& heap_;
  std::string_view text_;
  std::span<std::size_t> points_;
};

Result Compile(std::span<const Value> arguments, Heap& heap) {
  const auto source = arguments.front();
  const auto multi_line = arguments.subspan(1).front();
  const auto case_sensitive = arguments.subspan(2).front();
  const auto dot_all = arguments.subspan(3).front();
  if (!source.IsString() || !multi_line.IsBoolean() || !case_sensitive.IsBoolean() ||
      !dot_all.IsBoolean()) {
    return Failure(ExecutionStatus::TypeError);
  }
  const RegExpOptions options{
      .multi_line = multi_line.AsBoolean(),
      .case_sensitive = case_sensitive.AsBoolean(),
      .dot_all = dot_all.AsBoolean(),
  };
  const PcreOwner<pcre2_general_context, pcre2_general_context_free> general(
      pcre2_general_context_create(AllocatePcre, FreePcre, &heap), pcre2_general_context_free);
  if (!general) {
    return Failure(ExecutionStatus::OutOfMemory);
  }
  const PcreOwner<pcre2_compile_context, pcre2_compile_context_free> context(
      pcre2_compile_context_create(general.get()), pcre2_compile_context_free);
  if (!context) {
    return Failure(ExecutionStatus::OutOfMemory);
  }
  pcre2_set_newline(context.get(), PCRE2_NEWLINE_ANY);
  pcre2_set_bsr(context.get(), PCRE2_BSR_UNICODE);
  std::uint32_t flags = PCRE2_UTF | PCRE2_UCP | PCRE2_NEVER_BACKSLASH_C;
  flags |= options.multi_line ? PCRE2_MULTILINE : 0;
  flags |= options.case_sensitive ? 0 : PCRE2_CASELESS;
  flags |= options.dot_all ? PCRE2_DOTALL : 0;
  int error = 0;
  PCRE2_SIZE offset = 0;
  const auto text = source.AsString()->value();
  const void* pattern_bytes = text.data();
  PcreOwner<pcre2_code, pcre2_code_free> code(
      pcre2_compile(static_cast<PCRE2_SPTR>(pattern_bytes), text.size(), flags, &error, &offset,
                    context.get()),
      pcre2_code_free);
  if (!code) {
    if (error == PCRE2_ERROR_HEAP_FAILED) {
      return Failure(ExecutionStatus::OutOfMemory);
    }
    return Failure(ExecutionStatus::RegExpSyntaxError,
                   RegExpErrorMessage(error) + " at offset " +
                       std::to_string(Utf8Length(text.substr(0, offset))));
  }
  auto result = Allocated(
      heap.AllocateRegExp({.source = source.AsString(), .code = code.get(), .options = options}));
  if (result) {
    Heap::RegExpOf(*result->AsObject()).code = code.release();
  }
  return result;
}

Result Escape(Value input, Heap& heap) {
  if (!input.IsString()) {
    return Failure(ExecutionStatus::TypeError);
  }
  auto text = input.AsString()->value();
  constexpr std::size_t kMaximumEscapedScalar = 10;
  if (text.size() > kMaximumStringLength / kMaximumEscapedScalar) {
    return Failure(ExecutionStatus::OutOfMemory);
  }
  NativeBlock storage(heap, text.size() * kMaximumEscapedScalar);
  if (storage.data() == nullptr) {
    return Failure(ExecutionStatus::OutOfMemory);
  }
  auto buffer = std::span(static_cast<char*>(storage.data()), text.size() * kMaximumEscapedScalar);
  std::size_t size = 0;
  while (!text.empty()) {
    const auto decoded = DecodeUtf8Prefix(text);
    buffer.subspan(size++).front() = '\\';
    buffer.subspan(size++).front() = 'x';
    buffer.subspan(size++).front() = '{';
    const auto converted = std::to_chars(buffer.subspan(size).data(), std::to_address(buffer.end()),
                                         decoded.value, 16);
    size = static_cast<std::size_t>(converted.ptr - buffer.data());
    buffer.subspan(size++).front() = '}';
    text.remove_prefix(decoded.width);
  }
  return Text(heap, {buffer.data(), size});
}

std::unexpected<RegExpFailure> MatchFailure(int error) {
  if (error == PCRE2_ERROR_NOMEMORY) {
    return Failure(ExecutionStatus::OutOfMemory);
  }
  if (error == PCRE2_ERROR_MATCHLIMIT || error == PCRE2_ERROR_DEPTHLIMIT ||
      error == PCRE2_ERROR_HEAPLIMIT) {
    return Failure(ExecutionStatus::RegExpLimitError, RegExpErrorMessage(error));
  }
  return Failure(ExecutionStatus::RegExpExecutionError, RegExpErrorMessage(error));
}

Result Materialize(Heap& heap,
                   HeapObject& pattern,
                   StringObject& input,
                   pcre2_match_data& data,
                   Boundaries& boundaries) {
  if (const auto initialized = boundaries.Initialize(); !initialized) {
    return Failure(initialized.error());
  }
  const auto count = pcre2_get_ovector_count(&data);
  auto* object =
      heap.AllocateRegExpMatch({.pattern = &pattern, .input = &input, .capture_count = count});
  if (object == nullptr) {
    return Failure(ExecutionStatus::OutOfMemory);
  }
  const auto captures = Heap::RegExpMatchOf(*object).captures();
  const auto offsets =
      std::span(pcre2_get_ovector_pointer(&data), static_cast<std::size_t>(count) * 2);
  for (std::size_t index = 0; index < count; ++index) {
    const auto begin = offsets.subspan(index * 2).front();
    const auto end = offsets.subspan((index * 2) + 1).front();
    captures.subspan(index).front() = {
        .byte_start = begin,
        .byte_end = end,
        .start = begin == PCRE2_UNSET ? PCRE2_UNSET : boundaries.Point(begin),
        .end = end == PCRE2_UNSET ? PCRE2_UNSET : boundaries.Point(end),
    };
  }
  return Value::Object(object);
}

struct SearchState final {
  Heap* heap;
  HeapObject* pattern;
  StringObject* input;
  pcre2_match_data* data;
  Boundaries* boundaries;
};

Result MatchedValue(Builtin builtin, const SearchState& search) {
  if (builtin == Builtin::RegExpHasMatch) {
    return Value::Boolean(true);
  }
  if (builtin == Builtin::RegExpStringMatch) {
    const auto offsets = std::span(pcre2_get_ovector_pointer(search.data), 2);
    const auto begin = offsets.front();
    const auto end = offsets.subspan(1).front();
    const auto text = search.input->value();
    return begin == 0 && end == text.size() ? Result(Value::String(search.input))
                                            : Text(*search.heap, text.substr(begin, end - begin));
  }
  return Materialize(*search.heap, *search.pattern, *search.input, *search.data,
                     *search.boundaries);
}

Result Enumerate(Builtin builtin,
                 const SearchState& search,
                 std::size_t start,
                 const ArrayLayout* layout) {
  Value output = Value::Null();
  RootRange root(*search.heap, &output, 1, RootOwner::Runtime);
  if (builtin == Builtin::RegExpAllMatches) {
    const auto allocated = Allocated(search.heap->AllocateArray(*layout, 0));
    if (!allocated) {
      return allocated;
    }
    output = *allocated;
  }
  const auto text = search.input->value();
  const auto* code = Heap::RegExpOf(*search.pattern).code;
  const std::uint32_t flags =
      PCRE2_NO_UTF_CHECK | (builtin == Builtin::RegExpMatchAsPrefix ? PCRE2_ANCHORED : 0);
  const void* bytes = text.data();
  while (true) {
    const auto matched = pcre2_match(code, static_cast<PCRE2_SPTR>(bytes), text.size(), start,
                                     flags, search.data, nullptr);
    if (matched == PCRE2_ERROR_NOMATCH) {
      return builtin == Builtin::RegExpHasMatch ? Result(Value::Boolean(false)) : Result(output);
    }
    if (matched < 0) {
      return MatchFailure(matched);
    }
    const auto result = MatchedValue(builtin, search);
    if (!result || builtin != Builtin::RegExpAllMatches) {
      return result;
    }
    RootRange match_root(*search.heap, &*result, 1, RootOwner::Runtime);
    if (!search.heap->PushArray(*output.AsObject(), *result)) {
      return Failure(ExecutionStatus::OutOfMemory);
    }
    const auto offsets = std::span(pcre2_get_ovector_pointer(search.data), 2);
    const auto begin = offsets.front();
    const auto end = offsets.subspan(1).front();
    if (begin == end) {
      if (end == text.size()) {
        return output;
      }
      start = end + DecodeUtf8Prefix(text.substr(end)).width;
    } else {
      start = end;
    }
  }
}

std::expected<std::size_t, RegExpFailure> SearchStart(Builtin builtin,
                                                      std::span<const Value> arguments,
                                                      std::string_view text) {
  if (builtin != Builtin::RegExpAllMatches && builtin != Builtin::RegExpMatchAsPrefix) {
    return 0;
  }
  const auto position = arguments.subspan(2).front();
  if (!position.IsInteger()) {
    return Failure(ExecutionStatus::TypeError);
  }
  const auto index = position.AsInteger();
  if (index < 0) {
    return Failure(ExecutionStatus::IndexOutOfBounds);
  }
  const auto offset = Utf8Offset(text, static_cast<std::size_t>(index));
  if (!offset) {
    return Failure(ExecutionStatus::IndexOutOfBounds);
  }
  return *offset;
}

Result Search(Builtin builtin,
              std::span<const Value> arguments,
              Heap& heap,
              const ArrayLayout* layout) {
  const auto subject = arguments.subspan(1).front();
  if (!subject.IsString()) {
    return Failure(ExecutionStatus::TypeError);
  }
  auto& pattern = *arguments.front().AsObject();
  auto& input = *subject.AsString();
  Boundaries boundaries(heap, input.value());
  const auto start = SearchStart(builtin, arguments, input.value());
  if (!start) {
    return std::unexpected(start.error());
  }
  const PcreOwner<pcre2_match_data, pcre2_match_data_free> data(
      pcre2_match_data_create_from_pattern(Heap::RegExpOf(pattern).code, nullptr),
      pcre2_match_data_free);
  // match_data 从编译程序继承已核算的分配器，工作帧也由同一分配器释放。
  if (!data) {
    return Failure(ExecutionStatus::OutOfMemory);
  }
  return Enumerate(builtin,
                   {
                       .heap = &heap,
                       .pattern = &pattern,
                       .input = &input,
                       .data = data.get(),
                       .boundaries = &boundaries,
                   },
                   *start, layout);
}

Result Group(RegExpMatchStorage& match, std::int64_t index, Heap& heap) {
  if (index < 0 || std::cmp_greater_equal(index, match.capture_count)) {
    return Failure(ExecutionStatus::IndexOutOfBounds);
  }
  const auto& capture = match.captures().subspan(static_cast<std::size_t>(index)).front();
  if (capture.byte_start == PCRE2_UNSET) {
    return Value::Null();
  }
  if (capture.byte_start == 0 && capture.byte_end == match.input->value().size()) {
    return Value::String(match.input);
  }
  return Text(
      heap, match.input->value().substr(capture.byte_start, capture.byte_end - capture.byte_start));
}

struct Names final {
  std::span<const PCRE2_UCHAR> table;
  std::uint32_t width{};

  [[nodiscard]] std::string_view name(std::size_t entry) const {
    const auto bytes = table.subspan(entry * width, width).subspan(2);
    const auto end = std::ranges::find(bytes, 0);
    const void* name_bytes = bytes.data();
    return {static_cast<const char*>(name_bytes), static_cast<std::size_t>(end - bytes.begin())};
  }
  [[nodiscard]] std::uint32_t group(std::size_t entry) const {
    constexpr auto kBitsPerByte = 8;
    const auto bytes = table.subspan(entry * width, width);
    return (static_cast<std::uint32_t>(bytes.subspan(0).front()) << kBitsPerByte) |
           bytes.subspan(1).front();
  }
  [[nodiscard]] std::size_t count() const { return width == 0 ? 0 : table.size() / width; }
};

Names NameTable(RegExpMatchStorage& match) {
  const auto* code = Heap::RegExpOf(*match.pattern).code;
  std::uint32_t count = 0;
  std::uint32_t width = 0;
  PCRE2_SPTR table = nullptr;
  pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &count);
  pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &width);
  pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, static_cast<void*>(&table));
  return {.table = {table, static_cast<std::size_t>(count) * width}, .width = width};
}

Result NamedGroup(RegExpMatchStorage& match, Value input, Heap& heap) {
  if (!input.IsString()) {
    return Failure(ExecutionStatus::TypeError);
  }
  const auto name = input.AsString()->value();
  const auto names = NameTable(match);
  std::size_t low = 0;
  auto high = names.count();
  while (low < high) {
    const auto middle = low + ((high - low) / 2);
    if (names.name(middle) < name) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low == names.count() || names.name(low) != name) {
    return Failure(ExecutionStatus::RegExpGroupError);
  }
  for (auto entry = low; entry < names.count() && names.name(entry) == name; ++entry) {
    const auto group = names.group(entry);
    if (match.captures().subspan(group).front().byte_start != PCRE2_UNSET) {
      return Group(match, group, heap);
    }
  }
  return Value::Null();
}

Result Snapshot(Builtin builtin,
                RegExpMatchStorage& match,
                std::span<const Value> arguments,
                Heap& heap,
                const ArrayLayout& layout) {
  std::span<std::int64_t> indices;
  if (builtin == Builtin::RegExpMatchGroups) {
    if (!arguments.subspan(1).front().IsObject()) {
      return Failure(ExecutionStatus::TypeError);
    }
    const auto values = Heap::IntegerElements(*arguments.subspan(1).front().AsObject());
    if (!values) {
      return Failure(ExecutionStatus::TypeError);
    }
    indices = *values;
    for (const auto index : indices) {
      if (index < 0 || std::cmp_greater_equal(index, match.capture_count)) {
        return Failure(ExecutionStatus::IndexOutOfBounds);
      }
    }
  }
  auto output = Allocated(heap.AllocateArray(layout, 0));
  if (!output) {
    return output;
  }
  RootRange root(heap, &*output, 1, RootOwner::Runtime);
  const auto names = NameTable(match);
  const auto count = builtin == Builtin::RegExpMatchGroups ? indices.size() : names.count();
  std::string_view previous;
  for (std::size_t index = 0; index < count; ++index) {
    Result value = Value::Null();
    if (builtin == Builtin::RegExpMatchGroups) {
      value = Group(match, indices.subspan(index).front(), heap);
    } else {
      const auto name = names.name(index);
      if (index != 0 && previous == name) {
        continue;
      }
      previous = name;
      value = Text(heap, name);
    }
    if (!value) {
      return value;
    }
    RootRange value_root(heap, &*value, 1, RootOwner::Runtime);
    if (!heap.PushArray(*output->AsObject(), *value)) {
      return Failure(ExecutionStatus::OutOfMemory);
    }
  }
  return output;
}

Result PatternProperty(Builtin builtin, RegExpStorage& pattern) {
  switch (builtin) {
    case Builtin::RegExpPattern:
      return Value::String(pattern.source);
    case Builtin::RegExpIsMultiLine:
      return Value::Boolean(pattern.options.multi_line);
    case Builtin::RegExpIsCaseSensitive:
      return Value::Boolean(pattern.options.case_sensitive);
    case Builtin::RegExpIsDotAll:
      return Value::Boolean(pattern.options.dot_all);
    case Builtin::RegExpIsUnicode:
      return Value::Boolean(true);
    default:
      std::unreachable();
  }
}

Result MatchOperation(Builtin builtin,
                      std::span<const Value> arguments,
                      Heap& heap,
                      const ArrayLayout* layout) {
  auto& match = Heap::RegExpMatchOf(*arguments.subspan(0).front().AsObject());
  switch (builtin) {
    case Builtin::RegExpMatchStart:
      return Value::Integer(static_cast<std::int64_t>(match.captures().subspan(0).front().start));
    case Builtin::RegExpMatchEnd:
      return Value::Integer(static_cast<std::int64_t>(match.captures().subspan(0).front().end));
    case Builtin::RegExpMatchInput:
      return Value::String(match.input);
    case Builtin::RegExpMatchPattern:
      return Value::Object(match.pattern);
    case Builtin::RegExpMatchGroupCount:
      return Value::Integer(match.capture_count - 1);
    case Builtin::RegExpMatchGroupNames:
    case Builtin::RegExpMatchGroups:
      return Snapshot(builtin, match, arguments, heap, *layout);
    case Builtin::RegExpMatchNamedGroup:
      return NamedGroup(match, arguments.subspan(1).front(), heap);
    case Builtin::RegExpMatchGroup: {
      const auto index = arguments.subspan(1).front();
      return index.IsInteger() ? Group(match, index.AsInteger(), heap)
                               : Failure(ExecutionStatus::TypeError);
    }
    default:
      std::unreachable();
  }
}

}  // namespace

void WriteRegExp(std::ostream& output, HeapObject& object) {
  if (Heap::IsRegExp(object)) {
    output << "RegExp(pattern=" << Heap::RegExpOf(object).source->value() << ')';
    return;
  }
  auto& match = Heap::RegExpMatchOf(object);
  const auto& whole = match.captures().front();
  output << "RegExpMatch(start=" << whole.start << ", end=" << whole.end
         << ", groupCount=" << match.capture_count - 1 << ')';
}

Result ExecuteRegExpBuiltin(Builtin builtin,
                            std::span<const Value> arguments,
                            Heap& heap,
                            const ArrayLayout* output_layout) {
  RootRange roots(heap, arguments.data(), arguments.size(), RootOwner::Runtime);
  if (builtin == Builtin::NewRegExp) {
    return Compile(arguments, heap);
  }
  if (builtin == Builtin::RegExpEscape) {
    return Escape(arguments.subspan(0).front(), heap);
  }
  const auto receiver = arguments.subspan(0).front();
  if (!receiver.IsObject() || receiver.AsObject() == nullptr) {
    return Failure(ExecutionStatus::TypeError);
  }
  if (builtin >= Builtin::RegExpMatchStart) {
    return Heap::IsRegExpMatch(*receiver.AsObject())
               ? MatchOperation(builtin, arguments, heap, output_layout)
               : Failure(ExecutionStatus::TypeError);
  }
  if (!Heap::IsRegExp(*receiver.AsObject())) {
    return Failure(ExecutionStatus::TypeError);
  }
  if (builtin <= Builtin::RegExpIsUnicode) {
    return PatternProperty(builtin, Heap::RegExpOf(*receiver.AsObject()));
  }
  return Search(builtin, arguments, heap, output_layout);
}

}  // namespace vela::runtime
