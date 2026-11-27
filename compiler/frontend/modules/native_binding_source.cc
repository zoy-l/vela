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
    return std::unexpected("cannot read binding manifest: " + PathText(manifest_path) + ": " +
                           bytes.error());
  }
  const auto parsed = JsonDocument::Parse(*bytes);
  if (!parsed) {
    const auto& error = parsed.error();
    return std::unexpected("invalid binding manifest: " + PathText(manifest_path) + ": " +
                           error.message + " at byte " + std::to_string(error.offset));
  }
  const auto* root = parsed->root();
  const auto* version = GetJsonField(root, "format_version");
  const auto* generation = GetJsonField(root, "binding_generation");
  const auto* products = GetJsonField(root, "fingerprints");
  if (!yyjson_is_uint(version) || yyjson_get_uint(version) != 1) {
    return std::unexpected("unsupported binding manifest format_version: " +
                           PathText(manifest_path));
  }
  if (!yyjson_is_str(generation) || token != yyjson_get_str(generation)) {
    return std::unexpected("binding manifest generation does not match source: " +
                           PathText(manifest_path));
  }
  if (!yyjson_is_obj(products) || yyjson_obj_size(products) != 4) {
    return std::unexpected("binding manifest must contain four fingerprints: " +
                           PathText(manifest_path));
  }
  for (const std::string_view suffix : {".vela", "_bindings.h", "_bindings.cc", ".d"}) {
    const auto name = PathText(path.stem()) + std::string(suffix);
    const auto* expected = GetJsonField(products, name);
    if (!yyjson_is_str(expected)) {
      return std::unexpected("missing binding fingerprint for " + name + ": " +
                             PathText(manifest_path));
    }
    const auto product_path = path.parent_path() / Utf8Path(name);
    const auto product = ReadBinaryFile(product_path);
    if (!product) {
      return std::unexpected("cannot read binding product: " + PathText(product_path) + ": " +
                             product.error());
    }
    const std::string_view fingerprint = yyjson_get_str(expected);
    if (ContentFingerprint(*product).Hex() != fingerprint) {
      return std::unexpected("binding product fingerprint mismatch: " + PathText(product_path));
    }
    if (suffix == ".vela" && ContentFingerprint(source).Hex() != fingerprint) {
      return std::unexpected("captured binding source fingerprint mismatch: " + PathText(path));
    }
  }
  return {};
}

}  // namespace vela::frontend
