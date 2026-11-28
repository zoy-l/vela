#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <clang/Serialization/PCHContainerOperations.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/JSONCompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include "base/fingerprint.h"
#include "base/path.h"
#include "compiler/bindgen/collector.h"
#include "compiler/bindgen/emitter.h"
#include "compiler/bindgen/input_files.h"
#include "compiler/bindgen/model.h"
#include "compiler/frontend/syntax/names.h"

namespace vela::bindgen {
namespace {

struct Options final {
  llvm::cl::OptionCategory category{"Vela C++ binding collection"};
  llvm::cl::opt<std::string> module{"module", llvm::cl::Required, llvm::cl::cat(category)};
  llvm::cl::opt<std::string> database{"database", llvm::cl::Required, llvm::cl::cat(category)};
  llvm::cl::opt<std::string> source{"source", llvm::cl::Required, llvm::cl::cat(category)};
  llvm::cl::opt<std::string> resource{"resource-dir", llvm::cl::Required, llvm::cl::cat(category)};
  llvm::cl::opt<std::string> output{"output-directory", llvm::cl::Required,
                                    llvm::cl::cat(category)};
  llvm::cl::list<std::string> headers{"header", llvm::cl::OneOrMore, llvm::cl::cat(category)};
  llvm::cl::list<std::string> dependencies{"dependency", llvm::cl::ZeroOrMore,
                                           llvm::cl::cat(category)};
  llvm::cl::list<std::string> callbacks{"callback", llvm::cl::ZeroOrMore, llvm::cl::cat(category)};
  llvm::cl::list<std::string> opaque{"opaque", llvm::cl::ZeroOrMore, llvm::cl::cat(category)};
  llvm::cl::opt<unsigned> cpp_shards{"cpp-shards", llvm::cl::init(1), llvm::cl::cat(category)};
};

class SelectedDatabase final : public clang::tooling::CompilationDatabase {
 public:
  explicit SelectedDatabase(clang::tooling::CompileCommand command)
      : command_(std::move(command)) {}

  [[nodiscard]] std::vector<clang::tooling::CompileCommand> getCompileCommands(
      llvm::StringRef /*unused*/) const override {
    return {command_};
  }

 private:
  clang::tooling::CompileCommand command_;
};

std::expected<void, std::string> ReadDependency(const std::string& path, Collection& collection) {
  const auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    return std::unexpected("cannot read binding dependency: " + path);
  }
  auto json = llvm::json::parse((*buffer)->getBuffer());
  if (!json) {
    return std::unexpected("invalid binding dependency: " + path + ": " +
                           llvm::toString(json.takeError()));
  }
  const auto* object = json->getAsObject();
  const auto* bindings = object != nullptr ? object->getObject("bindings") : nullptr;
  const auto* classes = bindings != nullptr ? bindings->getArray("classes") : nullptr;
  if (classes == nullptr || bindings->getInteger("format_version") != 2) {
    return std::unexpected("unsupported binding dependency manifest: " + path);
  }
  const auto module = bindings->getString("module");
  if (!module || !frontend::IsIdentifier(*module)) {
    return std::unexpected("invalid dependency module: " + path);
  }
  const auto directory = Utf8Path(path).parent_path();
  Module::Dependency dependency{
      .module = module->str(),
      .source = PathText(directory / (module->str() + ".vela")),
      .header = PathText(directory / (module->str() + "_bindings.h")),
      .classes = {},
  };
  for (const auto& value : *classes) {
    const auto* type = value.getAsObject();
    const auto cpp = type != nullptr ? type->getString("cpp") : std::nullopt;
    const auto key = type != nullptr ? type->getString("key") : std::nullopt;
    const auto name = type != nullptr ? type->getString("name") : std::nullopt;
    if (!cpp || !key || !name || !frontend::IsNativeTypeKey(*key) ||
        !collection.dependencies.emplace(cpp->str(), key->str()).second) {
      return std::unexpected("invalid or duplicate host type in dependency: " + path);
    }
    dependency.classes.push_back({
        .key = key->str(),
        .name = name->str(),
        .cpp = cpp->str(),
        .documentation = {},
        .source = {},
        .members = {},
        .enumerators = {},
    });
  }
  collection.module.dependencies.push_back(std::move(dependency));
  return {};
}

std::expected<void, std::string> SelectDriver(clang::tooling::CompileCommand& command) {
  auto& arguments = command.CommandLine;
  if (arguments.empty()) {
    return std::unexpected("empty compilation command");
  }
  const auto wrapper = PathText(Utf8Path(arguments.front()).stem());
  if (wrapper == "sccache" || wrapper == "ccache") {
    arguments.erase(arguments.begin());
    if (!arguments.empty() && arguments.front() == "--") {
      arguments.erase(arguments.begin());
    }
  }
  const auto driver =
      arguments.empty() ? std::string{} : PathText(Utf8Path(arguments.front()).stem());
  if (driver != "clang" && driver != "clang++" && driver != "clang-cl") {
    return std::unexpected("unsupported binding parser driver: " + driver + "; expected Clang");
  }
  return {};
}

clang::tooling::CommandLineArguments AdjustArguments(
    const clang::tooling::CommandLineArguments& arguments,
    const std::string& resource_directory) {
  clang::tooling::CommandLineArguments result;
  bool output_argument = false;
  for (const auto& argument : arguments) {
    if (output_argument) {
      output_argument = false;
      continue;
    }
    const llvm::StringRef text(argument);
    if (text == "/Fo" || text == "/Fd" || text == "/Fa") {
      output_argument = true;
      continue;
    }
    if (text.starts_with("/Fo") || text.starts_with("/Fd") || text.starts_with("/Fa") ||
        text == "/showIncludes" || text == "/c") {
      continue;
    }
    result.push_back(argument);
  }
  result.emplace_back("-DVELA_BINDGEN=1");
  result.push_back("-resource-dir=" + resource_directory);
  return result;
}

int Collect(const Options& options) {
  if (!frontend::IsIdentifier(options.module.getValue())) {
    llvm::errs() << "module must be a non-keyword Vela identifier\n";
    return 1;
  }
  const auto cpp_shards = options.cpp_shards.getValue();
  if (cpp_shards == 0) {
    llvm::errs() << "cpp-shards must be positive\n";
    return 1;
  }
  std::string error;
  const auto commands = clang::tooling::JSONCompilationDatabase::loadFromFile(
      options.database.getValue(), error, clang::tooling::JSONCommandLineSyntax::AutoDetect);
  if (!commands) {
    llvm::errs() << error << '\n';
    return 1;
  }
  const auto source = Utf8Path(options.source.getValue());
  std::vector<clang::tooling::CompileCommand> selected;
  for (auto command : commands->getAllCompileCommands()) {
    std::error_code failure;
    auto path = Utf8Path(command.Filename);
    if (path.is_relative()) {
      path = Utf8Path(command.Directory) / path;
    }
    if (std::filesystem::equivalent(path, source, failure) && !failure) {
      selected.push_back(std::move(command));
    }
  }
  if (selected.size() != 1) {
    llvm::errs() << "translation unit must select exactly one compilation command; found "
                 << selected.size() << '\n';
    return 1;
  }
  auto command = std::move(selected.front());
  if (auto result = SelectDriver(command); !result) {
    llvm::errs() << result.error() << '\n';
    return 1;
  }
  Collection collection;
  collection.module.name = options.module.getValue();
  collection.output_directory = options.output.getValue();
  collection.module.source =
      PathText(Utf8Path(collection.output_directory) / (collection.module.name + ".vela"));
  collection.callbacks.insert(options.callbacks.begin(), options.callbacks.end());
  collection.opaque.insert(options.opaque.begin(), options.opaque.end());
  for (const auto& header : options.headers) {
    llvm::SmallString<0> path;
    if (const auto failure = llvm::sys::fs::real_path(header, path)) {
      llvm::errs() << "cannot resolve binding header: " << header << '\n';
      return 1;
    }
    collection.headers.insert(std::string(path));
  }
  for (const auto& dependency : options.dependencies) {
    if (auto result = ReadDependency(dependency, collection); !result) {
      llvm::errs() << result.error() << '\n';
      return 1;
    }
  }
  auto files = llvm::makeIntrusiveRefCnt<InputFileSystem>();
  const auto expanded = clang::tooling::expandResponseFiles(
      std::make_unique<SelectedDatabase>(std::move(command)), files);
  auto expanded_commands = expanded->getCompileCommands(options.source.getValue());
  auto expanded_command = std::move(expanded_commands.front());
  collection.module.directory = expanded_command.Directory;
  collection.module.arguments = expanded_command.CommandLine;
  SelectedDatabase input(std::move(expanded_command));
  clang::tooling::ClangTool tool(input, {options.source.getValue()},
                                 std::make_shared<clang::PCHContainerOperations>(), files);
  tool.appendArgumentsAdjuster([&options](const auto& arguments, llvm::StringRef /*unused*/) {
    return AdjustArguments(arguments, options.resource.getValue());
  });
  CollectorFactory factory(collection);
  if (tool.run(&factory) != 0) {
    return 1;
  }
  if (const auto failure = files->failure()) {
    llvm::errs() << "cannot resolve binding input: " << failure.message() << '\n';
    return 1;
  }
  collection.module.input_fingerprints = files->fingerprints();
  auto products =
      Emit(collection.module, collection.headers, options.output.getValue(), cpp_shards);
  if (!products) {
    llvm::errs() << products.error() << '\n';
    return 1;
  }
  auto serialized = Serialize(collection.module);
  serialized["products"] = std::move(*products);
  llvm::outs() << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(serialized)));
  return 0;
}

}  // namespace

int FingerprintFiles() {
  const auto input = llvm::MemoryBuffer::getSTDIN();
  if (!input) {
    llvm::errs() << "cannot read fingerprint request\n";
    return 1;
  }
  auto request = llvm::json::parse((*input)->getBuffer());
  if (!request) {
    llvm::errs() << llvm::toString(request.takeError());
    return 1;
  }
  const auto* paths = request->getAsArray();
  if (paths == nullptr) {
    llvm::errs() << "fingerprint request must be an array of paths\n";
    return 1;
  }
  llvm::json::Object output;
  for (const auto& value : *paths) {
    const auto path = value.getAsString();
    if (!path) {
      llvm::errs() << "fingerprint request must contain paths\n";
      return 1;
    }
    const auto file = llvm::MemoryBuffer::getFile(*path);
    if (!file) {
      llvm::errs() << "cannot fingerprint " << *path << '\n';
      return 1;
    }
    const auto buffer = (*file)->getBuffer();
    output[*path] = ContentFingerprint(std::string_view(buffer.data(), buffer.size())).Hex();
  }
  llvm::outs() << llvm::formatv("{0}\n", llvm::json::Value(std::move(output)));
  return 0;
}

}  // namespace vela::bindgen

int main(int argc, const char** argv) {
  llvm::InitLLVM initialization(argc, argv, /*InstallPipeSignalExitHandler=*/false);
  const auto arguments = std::span(argv, static_cast<std::size_t>(argc));
  if (argc == 2 && std::string_view(arguments[1]) == "fingerprint") {
    return vela::bindgen::FingerprintFiles();
  }
  vela::bindgen::Options options;
  llvm::cl::HideUnrelatedOptions(options.category);
  llvm::cl::ParseCommandLineOptions(argc, argv, "Collect annotated C++ classes for Vela\n");
  return vela::bindgen::Collect(options);
}
