#include "compiler/aot/artifact_publication.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <yyjson.h>

#include "base/callable.h"
#include "base/file.h"
#include "base/fingerprint.h"
#include "base/json.h"
#include "base/layout_description.h"
#include "base/path.h"
#include "compiler/aot/native_compiler.h"
#include "compiler/aot/target_support.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::aot {
namespace {

std::string_view ReadText(const yyjson_val* object, std::string_view key) {
  const auto* field = GetJsonField(object, key);
  return yyjson_is_str(field) ? std::string_view(yyjson_get_str(field), yyjson_get_len(field))
                              : std::string_view();
}

std::expected<void, std::string> VerifyLibraryIdentity(const yyjson_val* root) {
  const auto kind = ReadText(root, "kind");
  const auto optimization = ReadText(root, "optimization");
  if (optimization != "O2" && optimization != "O0") {
    return std::unexpected("unsupported native library optimization settings");
  }
  const auto toolchain_identity = NativeToolchainBuildIdentity(
      optimization == "O2" ? NativeOptimization::Release : NativeOptimization::None);
  if (yyjson_get_uint(GetJsonField(root, "format_version")) != 1 ||
      (kind != "obj" && kind != "staticlib") ||
      ReadText(root, "runtime_abi") != runtime::NativeRuntimeAbiIdentity() ||
      ReadText(root, "toolchain_build") != toolchain_identity ||
      ReadText(root, "target") != runtime::NativeTargetIdentity()) {
    return std::unexpected("native library format, target, or ABI identity mismatch");
  }
  return {};
}

class Manifest final {
 public:
  Manifest() : document_(json_.document()) {}

  [[nodiscard]] yyjson_mut_val* Text(std::string_view value) const {
    return yyjson_mut_strncpy(document_, value.data(), value.size());
  }
  [[nodiscard]] yyjson_mut_val* Number(std::uint64_t value) const {
    return yyjson_mut_uint(document_, value);
  }
  [[nodiscard]] yyjson_mut_val* Boolean(bool value) const {
    return yyjson_mut_bool(document_, value);
  }
  [[nodiscard]] yyjson_mut_val* Array() const { return yyjson_mut_arr(document_); }
  [[nodiscard]] yyjson_mut_val* Object() const { return yyjson_mut_obj(document_); }
  void Add(yyjson_mut_val* object, std::string_view key, yyjson_mut_val* field) const {
    AddJsonField(document_, object, key, field);
  }
  static void Append(yyjson_mut_val* array, yyjson_mut_val* value) {
    yyjson_mut_arr_append(array, value);
  }

  [[nodiscard]] yyjson_mut_val* Type(const layout::Type& type) const {
    auto* result = Object();
    Add(result, "kind", Number(static_cast<std::uint8_t>(type.kind)));
    Add(result, "nullable", Boolean(type.nullable));
    Add(result, "index", Number(type.index));
    return result;
  }

  [[nodiscard]] yyjson_mut_val* Signature(const CallSignature& signature) const {
    auto* result = Object();
    auto* parameters = Array();
    for (const auto& parameter : signature.parameters) {
      auto* value = Object();
      Add(value, "name", Text(parameter.name));
      Add(value, "type", Number(parameter.type));
      Add(value, "optional", Boolean(parameter.optional));
      Add(value, "rest", Boolean(parameter.rest));
      Append(parameters, value);
    }
    Add(result, "parameters", parameters);
    Add(result, "result",
        signature.result ? Number(*signature.result) : yyjson_mut_null(document_));
    return result;
  }

  void DescribeTypes(const ir::Program& program) const {
    const auto& layouts = program.layouts;
    auto* types = Array();
    for (std::size_t index = 0; index < layouts.types.size(); ++index) {
      auto* type = Type(layouts.types.at(index));
      Add(type, "name", Text(program.runtime_types.at(index).name));
      Append(types, type);
    }
    Add(json_.root(), "types", types);
    auto* arrays = Array();
    for (const auto& array : layouts.arrays) {
      Append(arrays, Type(array.element));
    }
    Add(json_.root(), "arrays", arrays);
    auto* collections = Array();
    for (const auto& collection : layouts.collections) {
      auto* value = Object();
      Add(value, "key", Type(collection.key));
      const auto& value_type = collection.value;
      Add(value, "value", value_type ? Type(*value_type) : yyjson_mut_null(document_));
      Append(collections, value);
    }
    Add(json_.root(), "collections", collections);
    auto* signatures = Array();
    for (const auto& signature : layouts.signatures) {
      auto* value = Object();
      auto* parameters = Array();
      for (const auto& parameter : signature.parameters) {
        auto* description = Object();
        Add(description, "type", Type(parameter.type));
        Add(description, "optional", Boolean(parameter.optional));
        Add(description, "rest", Boolean(parameter.rest));
        Append(parameters, description);
      }
      Add(value, "parameters", parameters);
      const auto& result_type = signature.result;
      Add(value, "result", result_type ? Type(*result_type) : yyjson_mut_null(document_));
      Append(signatures, value);
    }
    Add(json_.root(), "signatures", signatures);
    auto* classes = Array();
    for (const auto& layout : layouts.classes) {
      auto* value = Object();
      Add(value, "name", Text(layout.name));
      Add(value, "kind", Number(static_cast<std::uint8_t>(layout.kind)));
      Append(classes, value);
    }
    Add(json_.root(), "classes", classes);
    auto* interfaces = Array();
    for (const auto& name : layouts.interfaces) {
      Append(interfaces, Text(name));
    }
    Add(json_.root(), "interfaces", interfaces);
  }

  void DescribeFunctions(const ir::Program& program, const NativeArtifact& artifact) const {
    auto* functions = Array();
    for (std::size_t index = 0; index < program.functions.size(); ++index) {
      const auto& metric = artifact.function_metrics.at(index);
      auto* function = Object();
      Add(function, "name", Text(program.functions.at(index).name));
      Add(function, "shadow_slots", Number(metric.shadow_slots));
      Add(function, "frame_bytes", Number(metric.frame_bytes));
      Add(function, "root_stores", Number(metric.root_stores));
      Append(functions, function);
    }
    Add(json_.root(), "functions", functions);
    auto* exports = Array();
    for (const auto& entry : artifact.exports) {
      const auto& signature = program.functions.at(entry.function).signature;
      if (!signature) {
        std::unreachable();
      }
      auto* value = Signature(*signature);
      Add(value, "name", Text(entry.name));
      Add(value, "symbol", Text(entry.symbol));
      Add(value, "function", Number(entry.function));
      Append(exports, value);
    }
    Add(json_.root(), "exports", exports);
    auto* imports = Array();
    for (const auto& function : program.functions) {
      if (function.native_symbol.empty()) {
        continue;
      }
      const auto& signature = function.signature;
      if (!signature) {
        std::unreachable();
      }
      auto* value = Signature(*signature);
      Add(value, "name", Text(function.native_symbol));
      Append(imports, value);
    }
    Add(json_.root(), "imports", imports);
  }

  [[nodiscard]] std::expected<std::string, std::string> Serialize(
      const std::filesystem::path& output,
      const ir::Program& program,
      const NativeArtifact& artifact,
      std::span<const std::byte> binary,
      LibraryArtifactKind kind,
      const TargetSupport* support) const {
    auto* root = json_.root();
    Add(root, "format_version", Number(1));
    Add(root, "kind", Text(kind == LibraryArtifactKind::Object ? "obj" : "staticlib"));
    Add(root, "compilation_unit", Text(artifact.image_symbol));
    Add(root, "runtime_abi", Text(runtime::NativeRuntimeAbiIdentity()));
    Add(root, "toolchain_build", Text(NativeToolchainBuildIdentity(artifact.optimization)));
    Add(root, "target", Text(artifact.target));
    Add(root, "instantiate", Text(artifact.instantiate_symbol));
    Add(root, "image", Text(artifact.image_symbol));
    Add(root, "optimization",
        Text(artifact.optimization == NativeOptimization::Release ? "O2" : "O0"));
    Add(root, "cpu", Text("generic"));
    Add(root, "relocation", Text("pic"));
    const auto binary_hash = ContentFingerprint(binary).Hex();
    const auto header_hash = ContentFingerprint(artifact.header).Hex();
    Add(root, "generation", Text(ContentFingerprint(binary_hash + header_hash).Hex()));
    auto* files = Array();
    const auto name = PathText(output.filename());
    for (const auto& [role, path, fingerprint] : std::array{
             std::array<std::string, 3>{"binary", name, binary_hash},
             std::array<std::string, 3>{"header", name + ".h", header_hash},
         }) {
      auto* file = Object();
      Add(file, "role", Text(role));
      Add(file, "path", Text(path));
      Add(file, "xxh3_128", Text(fingerprint));
      Append(files, file);
    }
    Add(root, "files", files);
    DescribeFunctions(program, artifact);
    DescribeTypes(program);
    auto* libraries = Array();
    Append(libraries, Text("vela_runtime"));
    Add(root, "required_libraries", libraries);
    auto* link_order = Array();
    Append(link_order, Text(name));
    Append(link_order, Text("vela_runtime"));
    Add(root, "link_order", link_order);
    auto* target_support = Object();
    Add(target_support, "target", Text(artifact.target));
    Add(target_support, "runtime_variant", Text(runtime::NativeRuntimeAbiIdentity()));
    if (support != nullptr) {
      Add(target_support, "identity", Text(support->identity));
      auto* arguments = Array();
      for (const auto& argument : support->link_arguments) {
        Append(arguments, Text(argument));
      }
      Add(target_support, "link_arguments", arguments);
    }
    Add(root, "target_support", target_support);
    auto serialized = json_.Serialize();
    return serialized ? std::expected<std::string, std::string>(std::move(*serialized) + '\n')
                      : std::unexpected(serialized.error().message);
  }

 private:
  JsonOutput json_;
  yyjson_mut_doc* document_;
};

}  // namespace

std::expected<void, std::string> PublishNativeLibrary(const std::filesystem::path& output,
                                                      const NativeArtifact& artifact,
                                                      std::span<const std::byte> binary,
                                                      LibraryArtifactKind kind,
                                                      const TargetSupport* support) {
  const auto manifest =
      Manifest().Serialize(output, artifact.program, artifact, binary, kind, support);
  if (!manifest) {
    return std::unexpected(manifest.error());
  }
  auto header_path = output;
  header_path += ".h";
  auto manifest_path = output;
  manifest_path += ".aot.json";
  struct Publication final {
    std::filesystem::path path;
    std::span<const std::byte> bytes;
  };
  // 清单最后发布；I/O 失败可能留下混合输出，其文件指纹无法通过清单验证。
  const std::array publications{
      Publication{.path = output, .bytes = binary},
      Publication{.path = header_path, .bytes = std::as_bytes(std::span(artifact.header))},
      Publication{.path = manifest_path, .bytes = std::as_bytes(std::span(*manifest))},
  };
  for (const auto& publication : publications) {
    if (const auto written = WriteFileAtomically(publication.path, publication.bytes); !written) {
      return std::unexpected(PathText(publication.path) + ": " + written.error());
    }
  }
  return {};
}

std::expected<void, std::string> VerifyNativeLibrary(const std::filesystem::path& manifest) {
  const auto bytes = ReadBinaryFile(manifest);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  const auto parsed = JsonDocument::Parse(*bytes);
  if (!parsed) {
    return std::unexpected("invalid native library manifest: " + parsed.error().message);
  }
  const auto* root = parsed->root();
  if (const auto identity = VerifyLibraryIdentity(root); !identity) {
    return std::unexpected(identity.error());
  }
  const auto* files = GetJsonField(root, "files");
  if (!yyjson_is_arr(files) || yyjson_arr_size(files) != 2) {
    return std::unexpected("native library manifest requires binary and header identities");
  }
  std::string binary_hash;
  std::string header_hash;
  for (std::size_t index = 0; index < yyjson_arr_size(files); ++index) {
    const auto* file = yyjson_arr_get(files, index);
    const auto name = Utf8Path(ReadText(file, "path"));
    const auto expected = ReadText(file, "xxh3_128");
    if (name.empty() || name != name.filename() || expected.empty()) {
      return std::unexpected("invalid native library file identity");
    }
    const auto content = ReadBinaryFile(manifest.parent_path() / name);
    if (!content) {
      return std::unexpected(PathText(name) + ": " + content.error());
    }
    const auto hash = ContentFingerprint(*content).Hex();
    if (hash != expected) {
      return std::unexpected("native library files belong to a different generation: " +
                             PathText(name));
    }
    const auto role = ReadText(file, "role");
    if (role == "binary" && binary_hash.empty()) {
      binary_hash = hash;
    } else if (role == "header" && header_hash.empty()) {
      header_hash = hash;
    } else {
      return std::unexpected("invalid or duplicate native library file role");
    }
  }
  if (binary_hash.empty() || header_hash.empty() ||
      ContentFingerprint(binary_hash + header_hash).Hex() != ReadText(root, "generation")) {
    return std::unexpected("native library generation identity mismatch");
  }
  return {};
}

}  // namespace vela::compiler::aot
