#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include "compiler/build_info.h"
#include "compiler/frontend/checker.h"

namespace {

void PrintUsage(std::ostream& output) {
  output << "Usage: vela <command> [arguments]\n"
         << "\n"
         << "Commands:\n"
         << "  --help       Show this message\n"
         << "  --version    Show the compiler version\n"
         << "  check FILE   Parse and statically check a Vela source file\n";
}

int CheckFile(std::string_view path_text) {
  const std::string path(path_text);
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::cerr << "vela: cannot open source file '" << path << "'\n";
    return 1;
  }
  const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  const vela::frontend::CheckResult result = vela::frontend::CheckSource(source);
  for (const vela::frontend::Diagnostic& diagnostic : result.diagnostics) {
    std::cerr << path << ':' << diagnostic.location.line << ':' << diagnostic.location.column
              << ": error: " << diagnostic.message << '\n';
  }
  if (!result.Succeeded()) {
    return 1;
  }
  std::cout << "checked " << path << ": " << result.function_count << " function(s)\n";
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

  std::cerr << "vela: unknown command\n";
  PrintUsage(std::cerr);
  return 2;
}
