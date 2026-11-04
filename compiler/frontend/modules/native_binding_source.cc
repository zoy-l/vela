#include "compiler/frontend/modules/native_binding_source.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include <yyjson.h>

#include "base/file.h"
#include "base/fingerprint.h"
#include "base/json.h"
#include "base/path.h"

namespace vela::frontend {

std::expected<void, std::string> VerifyNativeBindingSource(const std::filesystem::path& path,
                                                           std::string_view source) {
  constexpr std::string_view kMarker = "// Binding generation: ";
  if (!source.starts_with(kMarker)) {
    return {};
  }
  const auto end = source.find('\n');
  const auto token = source.substr(kMarker.size(), end - kMarker.size());
  auto manifest_path = path;
  manifest_path.replace_extension(".bindings.json");
  const auto bytes = ReadBinaryFile(manifest_path);
  if (!bytes) {
    return std::unexpected("missing binding manifest: " + PathText(manifest_path));
  }
  const auto parsed = JsonDocument::Parse(*bytes);
  if (!parsed) {
    return std::unexpected("invalid binding manifest: " + PathText(manifest_path));
  }
  const auto* root = parsed->root();
  const auto* version = GetJsonField(root, "format_version");
  const auto* generation = GetJsonField(root, "binding_generation");
  const auto* products = GetJsonField(root, "fingerprints");
  if (!yyjson_is_uint(version) || yyjson_get_uint(version) != 1 || !yyjson_is_str(generation) ||
      token != yyjson_get_str(generation) || !yyjson_is_obj(products) ||
      yyjson_obj_size(products) != 4) {
    return std::unexpected("incomplete or mixed binding manifest: " + PathText(manifest_path));
  }
  for (const std::string_view suffix : {".vela", "_bindings.h", "_bindings.cc", ".d"}) {
    const auto name = PathText(path.stem()) + std::string(suffix);
    const auto* expected = GetJsonField(products, name);
    if (!yyjson_is_str(expected)) {
      return std::unexpected("incomplete binding manifest: " + PathText(manifest_path));
    }
    const auto product_path = path.parent_path() / Utf8Path(name);
    auto product = ReadBinaryFile(product_path);
    if (!product || ContentFingerprint(*product).Hex() != yyjson_get_str(expected) ||
        (suffix == std::string_view(".vela") &&
         ContentFingerprint(source).Hex() != yyjson_get_str(expected))) {
      return std::unexpected("stale or mixed binding product: " + PathText(product_path));
    }
  }
  return {};
}

}  // namespace vela::frontend
