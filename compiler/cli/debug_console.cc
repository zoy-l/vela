#include "compiler/cli/debug_console.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/container/btree_set.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <yyjson.h>

#include "base/execution_limits.h"
#include "base/json.h"
#include "base/path.h"
#include "compiler/cli/console_input.h"
#include "runtime/heap/heap.h"
#include "runtime/regexp/regexp.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_io.h"
#include "vm/debugger.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::tooling {

namespace {

enum class Mode : std::uint8_t { Entry, Continue, Step, Next, Finish, Instruction };
enum class Command : std::uint8_t {
  Help,
  Continue,
  Step,
  Next,
  Finish,
  Instruction,
  Break,
  Delete,
  Breakpoints,
  Stack,
  Frame,
  Registers,
  Print,
  Field,
  Element,
  Quit,
  Unknown,
};
enum class ConsoleAction : std::uint8_t { Prompt, Resume, Stop };

const absl::flat_hash_map<std::string_view, Command> kCommands = {
    {"help", Command::Help},         {"h", Command::Help},
    {"continue", Command::Continue}, {"c", Command::Continue},
    {"step", Command::Step},         {"s", Command::Step},
    {"next", Command::Next},         {"n", Command::Next},
    {"finish", Command::Finish},     {"f", Command::Finish},
    {"stepi", Command::Instruction}, {"si", Command::Instruction},
    {"break", Command::Break},       {"b", Command::Break},
    {"delete", Command::Delete},     {"breakpoints", Command::Breakpoints},
    {"stack", Command::Stack},       {"bt", Command::Stack},
    {"frame", Command::Frame},       {"registers", Command::Registers},
    {"regs", Command::Registers},    {"print", Command::Print},
    {"field", Command::Field},       {"element", Command::Element},
    {"quit", Command::Quit},         {"q", Command::Quit},
};

struct Point final {
  std::string_view path;
  std::string_view function;
  std::uint32_t line = 0;
  std::size_t depth = 0;
  std::size_t instruction = 0;
};

struct Breakpoint final {
  std::size_t identity = 0;
  std::string path;
  std::uint32_t line = 0;
  std::string function;
};

std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string_view Unquote(std::string_view text) {
  text = Trim(text);
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    text.remove_prefix(1);
    text.remove_suffix(1);
  }
  return text;
}

std::pair<std::string_view, std::string_view> Words(std::string_view text) {
  text = Trim(text);
  const auto separator = text.find_first_of(" \t");
  if (separator == std::string_view::npos) {
    return {text, {}};
  }
  return {text.substr(0, separator), Trim(text.substr(separator + 1))};
}

bool IsNewLine(const Point& current, const Point& previous) {
  return current.function != previous.function || current.path != previous.path ||
         current.line != previous.line || current.depth != previous.depth ||
         current.instruction <= previous.instruction;
}

Point CurrentPoint(const vm::DebugFrame& frame, std::size_t depth) {
  Point point{
      .path = std::string_view(),
      .function = frame.name(),
      .line = 0,
      .depth = depth,
      .instruction = frame.instruction(),
  };
  if (const auto location = frame.location()) {
    point.path = location->path;
    point.line = location->position.line;
  }
  return point;
}

void WriteFrame(std::ostream& output, const vm::DebugFrame& frame) {
  output << frame.name() << " pc=" << frame.instruction();
  if (const auto source = frame.location()) {
    output << " at " << source->path << ':' << source->position.line << ':'
           << source->position.column;
  }
  output << '\n';
}

void WriteDebugValue(std::ostream& output, runtime::Value value) {
  if (value.IsString()) {
    JsonOutput json;
    const auto text = value.AsString()->value();
    yyjson_mut_doc_set_root(json.document(),
                            yyjson_mut_strncpy(json.document(), text.data(), text.size()));
    const auto encoded = json.Serialize();
    if (encoded) {
      output << *encoded;
    } else {
      output << "<" << encoded.error().message << ">";
    }
  } else if (value.IsObject()) {
    const auto& object = *value.AsObject();
    if (const auto* layout = runtime::Heap::ClassOf(object)) {
      output << ((layout->callable() != nullptr) ? "function" : "object")
             << "(fields=" << layout->field_count() << ')';
    } else if (const auto length = runtime::Heap::ArrayLength(object)) {
      output << "array(length=" << *length << ')';
    } else if (const auto collection_size = runtime::Heap::CollectionLength(object)) {
      output << "collection(size=" << *collection_size << ')';
    } else if (runtime::Heap::IsMutableString(object)) {
      output << "String";
    } else if (runtime::Heap::IsRegExp(object) || runtime::Heap::IsRegExpMatch(object)) {
      runtime::WriteRegExp(output, *value.AsObject());
    } else if (runtime::Heap::IsByteArray(object)) {
      output << "ByteArray(length=" << runtime::Heap::ByteArrayBytes(*value.AsObject()).size()
             << ')';
    } else if (runtime::Heap::IsByteData(object)) {
      output << "ByteData(byteLength=" << runtime::Heap::ByteDataBytes(*value.AsObject()).size()
             << ')';
    } else {
      output << "object";
    }
  } else {
    runtime::WriteValue(output, value);
  }
}

class ConsoleDebugger final : public vm::Debugger {
 public:
  ConsoleDebugger(vm::Program program, std::istream& input, std::ostream& output)
      : program_(std::move(program)), input_(&input), output_(&output) {
    for (const auto& function : program_.functions()) {
      functions_.emplace(function.name);
      if (function.source_paths == nullptr) {
        continue;
      }
      for (const auto& mapping : function.source_mappings) {
        const auto& path = function.source_paths->at(mapping.location.source_id);
        source_lines_.try_emplace(path).first->second.insert(mapping.location.line);
      }
    }
    // source_lines_ 此后不再变化，文件名索引可安全借用其字符串键。
    for (const auto& [path, lines] : source_lines_) {
      source_names_.try_emplace(PathText(Utf8Path(path).filename())).first->second.push_back(path);
    }
  }
  vm::DebugAction BeforeInstruction(const vm::DebugContext& context) noexcept override;

 private:
  [[nodiscard]] bool ShouldStop(const Point& point) const;
  [[nodiscard]] bool IsBreakpoint(const Point& point) const;
  vm::DebugAction Prompt(const vm::DebugContext& context);
  ConsoleAction ExecuteCommand(Command command,
                               std::string_view arguments,
                               const vm::DebugContext& context,
                               vm::DebugFrame& selected);
  [[nodiscard]] std::expected<std::string, std::string> ResolveSource(std::string_view text) const;
  void AddBreakpoint(std::string_view text, const vm::DebugFrame& frame);
  void AddSourceBreakpoint(std::string_view path, std::uint32_t line);
  void DeleteBreakpoint(std::string_view text);
  void ListBreakpoints() const;
  void Stack(const vm::DebugContext& context) const;
  void SelectFrame(std::string_view text,
                   const vm::DebugContext& context,
                   vm::DebugFrame& selected) const;
  void Registers(const vm::DebugFrame& frame) const;
  void Inspect(Command command, std::string_view text, const vm::DebugFrame& frame) const;
  void Help() const;

  vm::Program program_;
  std::istream* input_;
  std::ostream* output_;
  Mode mode_ = Mode::Entry;
  Point last_stop_;
  std::array<std::optional<Point>, kMaxCallFrames> previous_;
  std::size_t next_breakpoint_ = 1;
  std::vector<Breakpoint> breakpoints_;
  absl::flat_hash_set<std::string_view> functions_;
  absl::flat_hash_map<std::string, absl::btree_set<std::uint32_t>> source_lines_;
  absl::flat_hash_map<std::string, std::vector<std::string_view>> source_names_;
  absl::flat_hash_map<std::string, absl::flat_hash_set<std::uint32_t>> source_breakpoints_;
  absl::flat_hash_set<std::string> function_breakpoints_;
};

bool ConsoleDebugger::ShouldStop(const Point& point) const {
  switch (mode_) {
    case Mode::Entry:
    case Mode::Instruction:
      return true;
    case Mode::Continue:
      return false;
    case Mode::Step:
      return IsNewLine(point, last_stop_);
    case Mode::Next:
      return point.depth <= last_stop_.depth && IsNewLine(point, last_stop_);
    case Mode::Finish:
      return point.depth < last_stop_.depth;
  }
  std::unreachable();
}

bool ConsoleDebugger::IsBreakpoint(const Point& point) const {
  if (point.instruction == 0 && function_breakpoints_.contains(point.function)) {
    return true;
  }
  const auto found = source_breakpoints_.find(point.path);
  return found != source_breakpoints_.end() && found->second.contains(point.line);
}

vm::DebugAction ConsoleDebugger::BeforeInstruction(const vm::DebugContext& context) noexcept {
  if (mode_ == Mode::Continue && breakpoints_.empty()) {
    return vm::DebugAction::Continue;
  }
  const auto current = context.current_frame();
  const auto point = CurrentPoint(current, context.frame_count());
  auto& previous = previous_.at(point.depth - 1);
  const auto entered = !previous || IsNewLine(point, *previous);
  previous = point;
  if (!ShouldStop(point) && !(entered && IsBreakpoint(point))) {
    return vm::DebugAction::Continue;
  }
  last_stop_ = point;
  WriteFrame(*output_, current);
  return Prompt(context);
}

void ConsoleDebugger::Help() const {
  *output_ << "continue (c), step (s), next (n), finish (f), stepi (si), quit (q)\n"
           << "break (b) [FILE:]LINE | FUNCTION; delete ID | all; breakpoints\n"
           << "stack (bt); frame DEPTH; registers (regs); print rN\n"
           << "field rN INDEX; element rN INDEX; help (h)\n";
}

vm::DebugAction ConsoleDebugger::Prompt(const vm::DebugContext& context) {
  auto selected = context.current_frame();
  while (true) {
    *output_ << "debug> " << std::flush;
    std::string line;
    if (!std::getline(*input_, line)) {
      return vm::DebugAction::Stop;
    }
    const auto [word, arguments] = Words(line);
    if (word.empty()) {
      continue;
    }
    const auto found = kCommands.find(word);
    const auto command = found == kCommands.end() ? Command::Unknown : found->second;
    const auto action = ExecuteCommand(command, arguments, context, selected);
    if (action == ConsoleAction::Resume) {
      return vm::DebugAction::Continue;
    }
    if (action == ConsoleAction::Stop) {
      return vm::DebugAction::Stop;
    }
  }
}

ConsoleAction ConsoleDebugger::ExecuteCommand(Command command,
                                              std::string_view arguments,
                                              const vm::DebugContext& context,
                                              vm::DebugFrame& selected) {
  switch (command) {
    case Command::Continue:
      mode_ = Mode::Continue;
      return ConsoleAction::Resume;
    case Command::Step:
      mode_ = Mode::Step;
      return ConsoleAction::Resume;
    case Command::Next:
      mode_ = Mode::Next;
      return ConsoleAction::Resume;
    case Command::Finish:
      mode_ = Mode::Finish;
      return ConsoleAction::Resume;
    case Command::Instruction:
      mode_ = Mode::Instruction;
      return ConsoleAction::Resume;
    case Command::Quit:
      return ConsoleAction::Stop;
    case Command::Help:
      Help();
      break;
    case Command::Break:
      AddBreakpoint(arguments, selected);
      break;
    case Command::Delete:
      DeleteBreakpoint(arguments);
      break;
    case Command::Breakpoints:
      ListBreakpoints();
      break;
    case Command::Stack:
      Stack(context);
      break;
    case Command::Frame:
      SelectFrame(arguments, context, selected);
      break;
    case Command::Registers:
      Registers(selected);
      break;
    case Command::Print:
    case Command::Field:
    case Command::Element:
      Inspect(command, arguments, selected);
      break;
    case Command::Unknown:
      *output_ << "unknown debugger command; use help\n";
      break;
  }
  return ConsoleAction::Prompt;
}

std::expected<std::string, std::string> ConsoleDebugger::ResolveSource(
    std::string_view text) const {
  text = Unquote(text);
  if (source_lines_.contains(text)) {
    return std::string(text);
  }
  const auto requested = Utf8Path(text);
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(requested, error);
  if (!error) {
    auto normalized = PathText(canonical);
    if (source_lines_.contains(normalized)) {
      return normalized;
    }
  }
  if (!requested.has_parent_path()) {
    const auto found = source_names_.find(text);
    if (found != source_names_.end()) {
      if (found->second.size() != 1) {
        return std::unexpected("source filename is ambiguous; use its full path");
      }
      return std::string(found->second.front());
    }
  }
  return std::unexpected("source has no executable locations in this program");
}

void ConsoleDebugger::AddSourceBreakpoint(std::string_view path, std::uint32_t line) {
  const auto source = ResolveSource(path);
  if (!source) {
    *output_ << source.error() << '\n';
    return;
  }
  const auto& lines = source_lines_.at(*source);
  const auto executable = lines.lower_bound(line);
  if (line == 0 || executable == lines.end()) {
    *output_ << "no executable line at or after the requested line\n";
    return;
  }
  auto& selected = source_breakpoints_.try_emplace(*source).first->second;
  if (!selected.insert(*executable).second) {
    *output_ << "breakpoint already exists\n";
    return;
  }
  const auto identity = next_breakpoint_++;
  breakpoints_.push_back({
      .identity = identity,
      .path = *source,
      .line = *executable,
      .function = std::string(),
  });
  *output_ << "breakpoint " << identity << " at " << *source << ':' << *executable << '\n';
}

void ConsoleDebugger::AddBreakpoint(std::string_view text, const vm::DebugFrame& frame) {
  text = Unquote(text);
  if (const auto line = ParseConsoleIndex(text)) {
    const auto source = frame.location();
    if (!source) {
      *output_ << "selected frame has no source mapping\n";
      return;
    }
    AddSourceBreakpoint(source->path, *line);
    return;
  }
  const auto colon = text.find_last_of(':');
  if (colon != std::string_view::npos) {
    const auto line = ParseConsoleIndex(text.substr(colon + 1));
    if (!line) {
      *output_ << "expected a positive line number after ':'\n";
      return;
    }
    AddSourceBreakpoint(text.substr(0, colon), *line);
    return;
  }
  if (!functions_.contains(text)) {
    *output_ << "unknown function or breakpoint location\n";
    return;
  }
  if (!function_breakpoints_.emplace(text).second) {
    *output_ << "breakpoint already exists\n";
    return;
  }
  const auto identity = next_breakpoint_++;
  breakpoints_.push_back({
      .identity = identity,
      .path = std::string(),
      .line = 0,
      .function = std::string(text),
  });
  *output_ << "breakpoint " << identity << " in " << text << '\n';
}

void ConsoleDebugger::DeleteBreakpoint(std::string_view text) {
  if (text == "all") {
    breakpoints_.clear();
    source_breakpoints_.clear();
    function_breakpoints_.clear();
    *output_ << "all breakpoints deleted\n";
    return;
  }
  const auto identity = ParseConsoleIndex(text);
  if (!identity) {
    *output_ << "delete requires a breakpoint ID or all\n";
    return;
  }
  const auto found = std::ranges::find(breakpoints_, *identity, &Breakpoint::identity);
  if (found == breakpoints_.end()) {
    *output_ << "unknown breakpoint ID\n";
    return;
  }
  if (found->function.empty()) {
    const auto source = source_breakpoints_.find(found->path);
    source->second.erase(found->line);
    if (source->second.empty()) {
      source_breakpoints_.erase(source);
    }
  } else {
    function_breakpoints_.erase(found->function);
  }
  breakpoints_.erase(found);
  *output_ << "breakpoint deleted\n";
}

void ConsoleDebugger::ListBreakpoints() const {
  if (breakpoints_.empty()) {
    *output_ << "no breakpoints\n";
  }
  for (const auto& breakpoint : breakpoints_) {
    *output_ << breakpoint.identity << ' ';
    if (breakpoint.function.empty()) {
      *output_ << breakpoint.path << ':' << breakpoint.line;
    } else {
      *output_ << breakpoint.function;
    }
    *output_ << '\n';
  }
}

void ConsoleDebugger::Stack(const vm::DebugContext& context) const {
  for (std::size_t depth = 0; const auto frame = context.Frame(depth); ++depth) {
    *output_ << '#' << depth << ' ';
    WriteFrame(*output_, *frame);
  }
}

void ConsoleDebugger::SelectFrame(std::string_view text,
                                  const vm::DebugContext& context,
                                  vm::DebugFrame& selected) const {
  const auto depth = ParseConsoleIndex(text);
  if (!depth) {
    *output_ << "frame requires a nonnegative depth\n";
    return;
  }
  const auto frame = context.Frame(*depth);
  if (!frame) {
    *output_ << "frame depth is out of range\n";
    return;
  }
  selected = *frame;
  WriteFrame(*output_, selected);
}

void ConsoleDebugger::Registers(const vm::DebugFrame& frame) const {
  for (std::size_t index = 0; const auto value = frame.ReadRegister(index); ++index) {
    *output_ << 'r' << index << " = ";
    WriteDebugValue(*output_, *value);
    *output_ << '\n';
  }
}

void ConsoleDebugger::Inspect(Command command,
                              std::string_view text,
                              const vm::DebugFrame& frame) const {
  auto [name, rest] = Words(text);
  if (!name.starts_with('r')) {
    *output_ << "expected a register name such as r0\n";
    return;
  }
  name.remove_prefix(1);
  const auto index = ParseConsoleIndex(name);
  if (!index) {
    *output_ << "invalid register name\n";
    return;
  }
  const auto value = frame.ReadRegister(*index);
  if (!value) {
    *output_ << "register is out of range\n";
    return;
  }
  if (command == Command::Print) {
    if (!rest.empty()) {
      *output_ << "print accepts one register name\n";
      return;
    }
    WriteDebugValue(*output_, *value);
    *output_ << '\n';
    return;
  }
  const auto element = ParseConsoleIndex(rest);
  if (!element || !value->IsObject()) {
    *output_ << "expected an object register and a nonnegative index\n";
    return;
  }
  const auto& object = *value->AsObject();
  if (command == Command::Field) {
    const auto* field = runtime::Heap::GetField(object, *element);
    if (field == nullptr) {
      *output_ << "object field is out of range or unavailable\n";
      return;
    }
    WriteDebugValue(*output_, *field);
  } else {
    const auto item = runtime::Heap::GetElement(object, *element);
    if (!item) {
      *output_ << "object is not an array or its index is out of range\n";
      return;
    }
    WriteDebugValue(*output_, *item);
  }
  *output_ << '\n';
}

}  // namespace

vm::ExecutionResult RunDebugConsole(const vm::FunctionHandle& function,
                                    runtime::Isolate& isolate,
                                    std::istream& input,
                                    std::ostream& output) {
  ConsoleDebugger debugger(function.program(), input, output);
  return vm::VirtualMachine(isolate).Debug(function, debugger);
}

}  // namespace vela::tooling
