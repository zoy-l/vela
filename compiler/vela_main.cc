#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

#include "compiler/build_info.h"
#include "compiler/compiler.h"
#include "compiler/frontend/checker.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace {

void PrintUsage(std::ostream& output) {
  output << "Usage: vela <command> [arguments]\n"
         << "\n"
         << "Commands:\n"
         << "  --help       Show this message\n"
         << "  --version    Show the compiler version\n"
         << "  check FILE   Parse and statically check a Vela source file\n"
         << "  run FILE     Compile and run the zero-argument main function\n"
         << "  bench FILE [ITERATIONS]\n"
         << "               Compile once and measure repeated VM execution\n";
}

void PrintDiagnostics(std::string_view path, const vela::frontend::Diagnostics& diagnostics) {
  for (const vela::frontend::Diagnostic& diagnostic : diagnostics) {
    std::cerr << path << ':' << diagnostic.location.line << ':' << diagnostic.location.column
              << ": error: " << diagnostic.message << '\n';
  }
}

bool ReadSource(std::string_view path_text, std::string* source) {
  std::ifstream input(std::string(path_text), std::ios::binary);
  if (!input) {
    return false;
  }
  source->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return true;
}

const vela::compiler::CompiledFunction* FindEntry(const vela::compiler::CompiledProgram& program,
                                                  std::size_t* entry_function) {
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

void PrintValue(const vela::runtime::Value& value) {
  if (value.IsInteger()) {
    std::cout << value.AsInteger();
  } else if (value.IsBoolean()) {
    std::cout << (value.AsBoolean() ? "true" : "false");
  } else if (value.IsNull()) {
    std::cout << "null";
  } else {
    std::cout << "object";
  }
}

int CheckFile(std::string_view path_text) {
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

const char* ExecutionStatusName(vela::vm::ExecutionStatus status) {
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
    case vela::vm::ExecutionStatus::IntegerOverflow:
      return "integer overflow";
    case vela::vm::ExecutionStatus::DivisionByZero:
      return "division by zero";
    case vela::vm::ExecutionStatus::StackOverflow:
      return "stack overflow";
  }
  return "unknown runtime error";
}

int RunFile(std::string_view path_text) {
  const std::string path(path_text);
  std::string source;
  if (!ReadSource(path, &source)) {
    std::cerr << "vela: cannot open source file '" << path << "'\n";
    return 1;
  }

  vela::runtime::Isolate isolate;
  const vela::compiler::CompileResult compiled = vela::compiler::CompileSource(source, isolate);
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
  PrintValue(result.value);
  std::cout << '\n';
  return 0;
}

bool ParseIterations(std::string_view text, std::size_t* iterations) {
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || value == 0 ||
      value > std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  *iterations = static_cast<std::size_t>(value);
  return true;
}

int BenchFile(std::string_view path_text, std::string_view iterations_text) {
  const std::string path(path_text);
  std::size_t iterations = 1000;
  if (!iterations_text.empty() && !ParseIterations(iterations_text, &iterations)) {
    std::cerr << "vela: benchmark iterations must be a positive integer\n";
    return 2;
  }

  std::string source;
  if (!ReadSource(path, &source)) {
    std::cerr << "vela: cannot open source file '" << path << "'\n";
    return 1;
  }

  vela::runtime::Isolate isolate;
  const auto compile_start = std::chrono::steady_clock::now();
  const vela::compiler::CompileResult compiled = vela::compiler::CompileSource(source, isolate);
  const auto compile_end = std::chrono::steady_clock::now();
  PrintDiagnostics(path, compiled.diagnostics);
  if (!compiled.Succeeded()) {
    return 1;
  }

  std::size_t entry_function = 0;
  const vela::compiler::CompiledFunction* function = FindEntry(compiled.program, &entry_function);
  if (function == nullptr || function->parameter_count != 0) {
    std::cerr << "vela: benchmark requires a zero-argument main function\n";
    return 1;
  }

  vela::vm::VirtualMachine vm(isolate);
  const auto first_start = std::chrono::steady_clock::now();
  const vela::vm::ExecutionResult first =
      vm.Execute(compiled.program.View(), static_cast<std::uint32_t>(entry_function));
  const auto first_end = std::chrono::steady_clock::now();
  if (!first.Succeeded()) {
    std::cerr << "vela: benchmark first execution failed: " << ExecutionStatusName(first.status)
              << '\n';
    return 1;
  }

  const std::size_t steady_iterations = iterations > 1 ? iterations - 1 : 0;
  const auto steady_start = std::chrono::steady_clock::now();
  vela::vm::ExecutionResult last = first;
  for (std::size_t index = 0; index < steady_iterations; ++index) {
    last = vm.Execute(compiled.program.View(), static_cast<std::uint32_t>(entry_function));
    if (!last.Succeeded()) {
      std::cerr << "vela: benchmark execution failed at iteration " << (index + 2) << ": "
                << ExecutionStatusName(last.status) << '\n';
      return 1;
    }
  }
  const auto steady_end = std::chrono::steady_clock::now();

  const auto milliseconds = [](auto duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
  };
  const auto nanoseconds = [](auto duration) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  };
  const auto steady_duration = steady_end - steady_start;
  std::cout << "benchmark " << path << '\n'
            << "iterations " << iterations << '\n'
            << "compile_ms " << milliseconds(compile_end - compile_start) << '\n'
            << "first_execute_ms " << milliseconds(first_end - first_start) << '\n'
            << "steady_iterations " << steady_iterations << '\n'
            << "steady_total_ms " << milliseconds(steady_duration) << '\n'
            << "steady_average_ns "
            << (steady_iterations == 0
                    ? 0
                    : nanoseconds(steady_duration) / static_cast<std::int64_t>(steady_iterations))
            << '\n'
            << "result ";
  PrintValue(last.value);
  std::cout << '\n'
            << "heap_objects " << isolate.heap().object_count() << '\n'
            << "allocated_bytes " << isolate.heap().allocated_bytes() << '\n'
            << "committed_bytes " << isolate.heap().committed_bytes() << '\n';
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 1 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
    PrintUsage(std::cout);
    return argc == 1 ? 2 : 0;
  }

  if (std::string_view(argv[1]) == "--version") {
    std::cout << vela::kProductName << ' ' << vela::kVersion << '\n';
    return 0;
  }

  if (std::string_view(argv[1]) == "check") {
    if (argc != 3) {
      std::cerr << "vela: check requires exactly one source file\n";
      PrintUsage(std::cerr);
      return 2;
    }
    return CheckFile(argv[2]);
  }

  if (std::string_view(argv[1]) == "run") {
    if (argc != 3) {
      std::cerr << "vela: run requires exactly one source file\n";
      PrintUsage(std::cerr);
      return 2;
    }
    return RunFile(argv[2]);
  }

  if (std::string_view(argv[1]) == "bench") {
    if (argc != 3 && argc != 4) {
      std::cerr << "vela: bench requires a source file and optional iteration count\n";
      PrintUsage(std::cerr);
      return 2;
    }
    return BenchFile(argv[2], argc == 4 ? std::string_view(argv[3]) : std::string_view());
  }

  std::cerr << "vela: unknown command\n";
  PrintUsage(std::cerr);
  return 2;
}
