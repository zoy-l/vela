#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

#include "compiler/build_info.h"
#include "compiler/compiler.h"
#include "compiler/frontend/checker.h"
#include "runtime/isolate.h"
#include "runtime/value_io.h"
#include "vm/vm.h"

namespace {

void PrintUsage(std::ostream& output) {
  output << "Usage: vela <command> [arguments]\n"
         << "\n"
         << "Commands:\n"
         << "  --help       Show this message\n"
         << "  --version    Show the compiler version\n"
         << "  check FILE   Parse and statically check a Vela source file\n"
         << "  run FILE     Compile and run the zero-argument main function\n";
}

void PrintDiagnostics(std::string_view path, const vela::frontend::Diagnostics& diagnostics) {
  for (const vela::frontend::Diagnostic& diagnostic : diagnostics) {
    std::cerr << path << ':' << diagnostic.location.line << ':' << diagnostic.location.column
              << ": error: " << diagnostic.message << '\n';
  }
}

auto ReadSource(std::string_view path_text, std::string* source) -> bool {
  std::ifstream input(std::string(path_text), std::ios::binary);
  if (!input) {
    return false;
  }
  source->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return true;
}

auto FindEntry(const vela::compiler::CompiledProgram& program, std::size_t* entry_function)
    -> const vela::compiler::CompiledFunction* {
  const vela::compiler::CompiledFunction* function = program.FindFunction("main");
  if (function == nullptr && program.functions.size() == 1) {
    function = &program.functions.front();
  }
  if (function == nullptr) {
    return nullptr;
  }
  *entry_function = static_cast<std::size_t>(function - program.functions.data());
  return function;
}

auto CheckFile(std::string_view path_text) -> int {
  const std::string path(path_text);
  std::string source;
  if (!ReadSource(path, &source)) {
    std::cerr << "vela: cannot open source file '" << path << "'\n";
    return 1;
  }
  const vela::frontend::CheckResult result = vela::frontend::CheckSource(source);
  PrintDiagnostics(path, result.diagnostics);
  if (!result.Succeeded()) {
    return 1;
  }
  std::cout << "checked " << path << ": " << result.function_count << " function(s)\n";
  return 0;
}

auto ExecutionStatusName(vela::vm::ExecutionStatus status) -> const char* {
  switch (status) {
    case vela::vm::ExecutionStatus::Returned:
      return "returned";
    case vela::vm::ExecutionStatus::InvalidBytecode:
      return "invalid bytecode";
    case vela::vm::ExecutionStatus::OutOfMemory:
      return "out of memory";
    case vela::vm::ExecutionStatus::TypeError:
      return "runtime type error";
    case vela::vm::ExecutionStatus::InvalidField:
      return "invalid field";
    case vela::vm::ExecutionStatus::IndexOutOfBounds:
      return "array index out of bounds";
    case vela::vm::ExecutionStatus::IntegerOverflow:
      return "integer overflow";
    case vela::vm::ExecutionStatus::DivisionByZero:
      return "division by zero";
    case vela::vm::ExecutionStatus::StackOverflow:
      return "stack overflow";
  }
  return "unknown runtime error";
}

auto RunFile(std::string_view path_text) -> int {
  const std::string path(path_text);
  std::string source;
  if (!ReadSource(path, &source)) {
    std::cerr << "vela: cannot open source file '" << path << "'\n";
    return 1;
  }

  vela::runtime::Isolate isolate;
  const vela::compiler::CompileResult compiled = vela::compiler::CompileFile(path, isolate);
  PrintDiagnostics(path, compiled.diagnostics);
  if (!compiled.Succeeded()) {
    return 1;
  }

  std::size_t entry_function = 0;
  const vela::compiler::CompiledFunction* function = FindEntry(compiled.program, &entry_function);
  if (function == nullptr) {
    std::cerr << "vela: run requires a zero-argument main function\n";
    return 1;
  }
  if (function->parameter_count != 0) {
    std::cerr << "vela: run does not accept main function parameters yet\n";
    return 1;
  }

  const vela::vm::ExecutionResult result = vela::vm::VirtualMachine(isolate).Execute(
      compiled.program.View(), static_cast<std::uint32_t>(entry_function));
  if (!result.Succeeded()) {
    std::cerr << "vela: " << ExecutionStatusName(result.status) << '\n';
    return 1;
  }
  vela::runtime::WriteValue(std::cout, result.value);
  std::cout << '\n';
  return 0;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
  if (argc == 1 || std::string_view(arguments.subspan(1).front()) == "--help" ||
      std::string_view(arguments.subspan(1).front()) == "-h") {
    PrintUsage(std::cout);
    return argc == 1 ? 2 : 0;
  }

  if (std::string_view(arguments.subspan(1).front()) == "--version") {
    std::cout << vela::kProductName << ' ' << vela::kVersion << '\n';
    return 0;
  }

  if (std::string_view(arguments.subspan(1).front()) == "check") {
    if (argc != 3) {
      std::cerr << "vela: check requires exactly one source file\n";
      PrintUsage(std::cerr);
      return 2;
    }
    return CheckFile(arguments.subspan(2).front());
  }

  if (std::string_view(arguments.subspan(1).front()) == "run") {
    if (argc != 3) {
      std::cerr << "vela: run requires exactly one source file\n";
      PrintUsage(std::cerr);
      return 2;
    }
    return RunFile(arguments.subspan(2).front());
  }

  std::cerr << "vela: unknown command\n";
  PrintUsage(std::cerr);
  return 2;
}
