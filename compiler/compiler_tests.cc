#include <cstdint>
#include <iostream>
#include <string_view>

#include "compiler/compiler.h"
#include "runtime/heap.h"

namespace {

using vela::compiler::CompileSource;
using vela::runtime::Isolate;
using vela::runtime::RootRange;
using vela::runtime::Value;
using vela::vm::ExecutionStatus;
using vela::vm::VirtualMachine;

bool TestCompilesIntegerFunction() {
  constexpr std::string_view source = R"(
int main() {
  int x = 20;
  int y = 22;
  return x + y;
}
)";

  Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  if (!compiled.Succeeded()) {
    return false;
  }
  const auto* function = compiled.program.FindFunction("main");
  if (function == nullptr) {
    return false;
  }
  const auto result = VirtualMachine(isolate).Execute(function->View());
  return result.status == ExecutionStatus::Returned && result.value.IsInteger() &&
         result.value.AsInteger() == 42;
}

bool TestCompilesNestedClassConstruction() {
  constexpr std::string_view source = R"(
class Theme {
  int brightness;
}

class App {
  Theme theme;
}

App main() {
  return App(theme: Theme(brightness: 1));
}
)";

  Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  if (!compiled.Succeeded()) {
    return false;
  }
  const auto* function = compiled.program.FindFunction("main");
  if (function == nullptr) {
    return false;
  }
  const auto result = VirtualMachine(isolate).Execute(function->View());
  if (result.status != ExecutionStatus::Returned || !result.value.IsObject() ||
      result.value.AsObject() == nullptr) {
    return false;
  }

  Value result_root = result.value;
  RootRange root(isolate.heap(), &result_root, 1);
  const Value* theme = isolate.heap().GetField(*result_root.AsObject(), 0);
  if (theme == nullptr || !theme->IsObject() || theme->AsObject() == nullptr) {
    return false;
  }
  const Value* brightness = isolate.heap().GetField(*theme->AsObject(), 0);
  if (brightness == nullptr || !brightness->IsInteger() || brightness->AsInteger() != 1) {
    return false;
  }

  result_root = Value::Null();
  isolate.heap().Collect();
  return isolate.heap().object_count() == 0;
}

bool TestCompilesFunctionCall() {
  constexpr std::string_view source = R"(
int add(int left, int right) {
  return left + right;
}

int main() {
  return add(20, 22);
}
)";

  Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  if (!compiled.Succeeded()) {
    return false;
  }
  const auto* function = compiled.program.FindFunction("main");
  if (function == nullptr) {
    return false;
  }
  const std::uint32_t entry =
      static_cast<std::uint32_t>(function - compiled.program.functions.data());
  const auto result = VirtualMachine(isolate).Execute(compiled.program.View(), entry);
  return result.status == ExecutionStatus::Returned && result.value.IsInteger() &&
         result.value.AsInteger() == 42;
}

bool TestCompilesLoopControlFlow() {
  constexpr std::string_view source = R"(
int main() {
  int i = 0;
  int sum = 0;
  while (i != 5) {
    sum = sum + i;
    i = i + 1;
  }
  return sum;
}
)";

  Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  if (!compiled.Succeeded()) {
    return false;
  }
  const auto* function = compiled.program.FindFunction("main");
  if (function == nullptr) {
    return false;
  }
  const auto result = VirtualMachine(isolate).Execute(function->View());
  return result.status == ExecutionStatus::Returned && result.value.IsInteger() &&
         result.value.AsInteger() == 10;
}

}  // namespace

int main() {
  if (!TestCompilesIntegerFunction() || !TestCompilesNestedClassConstruction() ||
      !TestCompilesFunctionCall() || !TestCompilesLoopControlFlow()) {
    std::cerr << "compiler tests failed\n";
    return 1;
  }
  return 0;
}
