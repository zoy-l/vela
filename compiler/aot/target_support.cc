#include "compiler/aot/target_support.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/SHA256.h>
#include <yyjson.h>

#include "base/file.h"
#include "base/fingerprint.h"
#include "base/json.h"
#include "base/path.h"
#include "compiler/aot/native_compiler.h"
#include "compiler/native/code_generator.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::aot {
namespace {

std::string_view Text(const yyjson_val* object, std::string_view key) {
  const auto* value = GetJsonField(object, key);
  return yyjson_is_str(value) ? std::string_view(yyjson_get_str(value), yyjson_get_len(value))
                              : std::string_view();
}

std::expected<std::filesystem::path, std::string>
PackagedPath(const yyjson_val* root, std::string_view key, const std::filesystem::path& package) {
  const auto relative = Utf8Path(Text(root, key));
  if (relative.empty() || relative.has_root_path() ||
      std::ranges::any_of(relative, [](const auto& part) { return part == ".."; })) {
    return std::unexpected("target support requires a package-relative path for " +
                           std::string(key));
  }
  return package / relative;
}

using Roots = absl::flat_hash_map<std::string, std::string>;

std::string Expand(std::string argument, const Roots& roots) {
  for (const auto& [key, value] : roots) {
    std::size_t position = 0;
    while ((position = argument.find(key, position)) != std::string::npos) {
      argument.replace(position, key.size(), value);
      position += value.size();
    }
  }
  return argument;
}

std::expected<Roots, std::string> ResolveRoots(const yyjson_val* root,
                                               const std::filesystem::path& package,
                                               NativeOutputKind kind) {
  Roots roots{{"{package}", PathText(package)}};
  if (kind != NativeOutputKind::Executable) {
    return roots;
  }
  const auto* external = GetJsonField(root, "external_roots");
  if (!yyjson_is_arr(external)) {
    return std::unexpected("target support requires external_roots provenance");
  }
  for (std::size_t index = 0; index < yyjson_arr_size(external); ++index) {
    const auto* entry = yyjson_arr_get(external, index);
    const auto name = Text(entry, "name");
    const auto environment = Text(entry, "environment");
    if (name.empty() || environment.empty() || Text(entry, "version").empty() ||
        Text(entry, "source").empty() || Text(entry, "license").empty() ||
        Text(entry, "instructions").empty()) {
      return std::unexpected(
          "external SDK root requires version, provenance, license, and instructions");
    }
    const auto value = llvm::sys::Process::GetEnv(environment);
    if (!value || value->empty()) {
      return std::unexpected("configure " + std::string(environment) + ": " +
                             std::string(Text(entry, "instructions")));
    }
    const auto path = Utf8Path(*value);
    if (!path.is_absolute()) {
      return std::unexpected(std::string(environment) + " must be an absolute SDK path");
    }
    roots.emplace("{" + std::string(name) + "}", PathText(path));
  }
  return roots;
}

std::expected<void, std::string> VerifyFile(const Roots& roots, const yyjson_val* file) {
  const auto name = Text(file, "path");
  const auto expected = Text(file, "sha256");
  if (name.empty() || expected.empty()) {
    return std::unexpected("target support file requires path and sha256");
  }
  const auto expanded = Expand(std::string(name), roots);
  const auto path = Utf8Path(expanded);
  if (!path.is_absolute() || expanded.contains('{')) {
    return std::unexpected("unresolved target support path: " + expanded);
  }
  const auto bytes = ReadBinaryFile(path);
  if (!bytes) {
    return std::unexpected(PathText(path) + ": " + bytes.error());
  }
  llvm::SHA256 digest;
  const void* storage = bytes->data();
  digest.update(llvm::StringRef(static_cast<const char*>(storage), bytes->size()));
  if (llvm::toHex(digest.final(), true) != expected) {
    return std::unexpected("target support file identity mismatch: " + PathText(path));
  }
  return {};
}

std::expected<std::vector<std::string>, std::string> Arguments(const yyjson_val* root,
                                                               std::string_view key,
                                                               const Roots& roots) {
  const auto* array = GetJsonField(root, key);
  if (!yyjson_is_arr(array) || yyjson_arr_size(array) == 0) {
    return std::unexpected("target support requires argument array: " + std::string(key));
  }
  std::vector<std::string> result;
  for (std::size_t index = 0; index < yyjson_arr_size(array); ++index) {
    const auto* value = yyjson_arr_get(array, index);
    if (!yyjson_is_str(value)) {
      return std::unexpected("target support arguments must be strings");
    }
    result.push_back(Expand(std::string(yyjson_get_str(value), yyjson_get_len(value)), roots));
  }
  return result;
}

class LinkWorkspace final {
 public:
  explicit LinkWorkspace(std::filesystem::path path) : path_(std::move(path)) {}
  ~LinkWorkspace() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  LinkWorkspace(const LinkWorkspace&) = delete;
  LinkWorkspace& operator=(const LinkWorkspace&) = delete;
  LinkWorkspace(LinkWorkspace&&) = delete;
  LinkWorkspace& operator=(LinkWorkspace&&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

std::expected<void, std::string> InvokeTool(const std::filesystem::path& tool,
                                            std::span<const std::string> arguments,
                                            const std::filesystem::path& log) {
  const auto executable = PathText(tool);
  std::vector<llvm::StringRef> references{executable};
  references.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) {
    references.emplace_back(argument);
  }
  const auto log_path = PathText(log);
  const std::array<std::optional<llvm::StringRef>, 3> redirects{
      llvm::StringRef(),
      llvm::StringRef(log_path),
      llvm::StringRef(log_path),
  };
  std::string error;
  const auto code =
      llvm::sys::ExecuteAndWait(executable, references, std::nullopt, redirects, 0, 0, &error);
  if (code == 0) {
    return {};
  }
  const auto bytes = ReadBinaryFile(log);
  if (bytes) {
    const void* storage = bytes->data();
    error.append(static_cast<const char*>(storage), bytes->size());
  }
  return std::unexpected(PathText(tool.filename()) + " failed (exit " + std::to_string(code) +
                         "): " + error);
}

}  // namespace

std::filesystem::path DefaultTargetSupportPath() {
  return Utf8Path(llvm::sys::fs::getMainExecutable("vela", nullptr)).parent_path().parent_path() /
         "share" / "vela" / "target.json";
}

std::expected<TargetSupport, std::string> LoadTargetSupport(const std::filesystem::path& manifest,
                                                            NativeOutputKind kind) {
  const auto bytes = ReadBinaryFile(manifest);
  if (!bytes) {
    return std::unexpected(
        "missing target support " + PathText(manifest) +
        "; install a matching Vela distribution or use --target-support=FILE: " + bytes.error());
  }
  const auto parsed = JsonDocument::Parse(*bytes);
  if (!parsed) {
    return std::unexpected("invalid target support JSON: " + parsed.error().message);
  }
  const auto* root = parsed->root();
  if (yyjson_get_uint(GetJsonField(root, "format_version")) != 1 ||
      Text(root, "target") != runtime::NativeTargetIdentity() ||
      Text(root, "runtime_abi") != runtime::NativeRuntimeAbiIdentity() ||
      Text(root, "toolchain_build") != NativeToolchainBuildIdentity(NativeOptimization::Release)) {
    return std::unexpected("target support target or runtime ABI does not match this compiler");
  }
  std::error_code error;
  const auto package = std::filesystem::absolute(
                           manifest.parent_path() / Utf8Path(Text(root, "package_root")), error)
                           .lexically_normal();
  if (error) {
    return std::unexpected("cannot resolve target support package: " + error.message());
  }
  const auto roots = ResolveRoots(root, package, kind);
  if (!roots) {
    return std::unexpected(roots.error());
  }
  const auto* files = GetJsonField(root, "files");
  if (!yyjson_is_arr(files)) {
    return std::unexpected("target support requires files and their identities");
  }
  for (std::size_t index = 0; index < yyjson_arr_size(files); ++index) {
    const auto* file = yyjson_arr_get(files, index);
    if (kind != NativeOutputKind::Executable && Text(file, "role") == "link") {
      continue;
    }
    const auto verified = VerifyFile(*roots, file);
    if (!verified) {
      return std::unexpected(verified.error());
    }
  }
  auto archive = Arguments(root, "archive_arguments", *roots);
  if (!archive) {
    return std::unexpected(archive.error());
  }
  auto link = Arguments(root, "link_arguments", *roots);
  if (!link) {
    return std::unexpected(link.error());
  }
  const auto archiver = PackagedPath(root, "archiver", package);
  const auto linker = PackagedPath(root, "linker", package);
  const auto runtime = PackagedPath(root, "runtime", package);
  for (const auto* path : {&archiver, &linker, &runtime}) {
    if (!*path) {
      return std::unexpected(path->error());
    }
  }
  return TargetSupport{
      .package = package,
      .archiver = *archiver,
      .linker = *linker,
      .runtime = *runtime,
      .identity = ContentFingerprint(*bytes).Hex(),
      .archive_arguments = std::move(*archive),
      .link_arguments = std::move(*link),
  };
}

std::expected<std::vector<std::byte>, std::string> LinkNativeArtifact(
    const TargetSupport& support,
    std::span<const std::byte> object,
    NativeOutputKind kind) {
  constexpr std::size_t kInitialPathCapacity = 128;
  llvm::SmallString<kInitialPathCapacity> directory;
  std::error_code temporary_error;
  const auto temporary = std::filesystem::temp_directory_path(temporary_error);
  if (temporary_error) {
    return std::unexpected("cannot resolve temporary directory: " + temporary_error.message());
  }
  if (const auto error =
          llvm::sys::fs::createUniqueDirectory(PathText(temporary / "vela-aot"), directory)) {
    return std::unexpected("cannot create link workspace: " + error.message());
  }
  const LinkWorkspace work(Utf8Path(directory.str().str()));
  const auto input = work.path() / "unit.o";
  const auto output = work.path() / "output";
  if (const auto written = WriteFileAtomically(input, object); !written) {
    return std::unexpected(written.error());
  }
  auto arguments =
      kind == NativeOutputKind::StaticLibrary ? support.archive_arguments : support.link_arguments;
  const Roots substitutions{
      {"{object}", PathText(input)},
      {"{output}", PathText(output)},
      {"{runtime}", PathText(support.runtime)},
  };
  for (auto& argument : arguments) {
    argument = Expand(std::move(argument), substitutions);
  }
  const auto& tool = kind == NativeOutputKind::StaticLibrary ? support.archiver : support.linker;
  if (const auto executed = InvokeTool(tool, arguments, work.path() / "tool.log"); !executed) {
    return std::unexpected(executed.error());
  }
  return ReadBinaryFile(output);
}

}  // namespace vela::compiler::aot
