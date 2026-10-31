#include "compiler/compilation_cache.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "base/binary.h"
#include "base/file.h"
#include "base/fingerprint.h"
#include "base/path.h"
#include "compiler/cache_build_identity.h"
#include "compiler/compiler.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/sdk_modules.h"
#include "compiler/frontend/source_file.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/standard_environment.h"
#include "runtime/isolate.h"
#include "vm/bytecode_image.h"

namespace vela::compiler {
namespace {

constexpr std::string_view kCacheMagic = "VELACCH3";

struct InputIdentity final {
  std::string path;
  Fingerprint fingerprint;
};

struct CacheImage final {
  std::string build_identity;
  std::string entry;
  frontend::DiagnosticOptions options;
  frontend::Diagnostics diagnostics;
  std::vector<InputIdentity> inputs;
  std::vector<frontend::FileResolution> resolutions;
  std::span<const std::byte> bytecode;
};

std::expected<CacheImage, BinaryError> ReadImage(BinaryReader& reader) {
  auto build_identity = reader.Text();
  auto entry = reader.Text();
  const auto severity = reader.Record<std::uint8_t>();
  const auto count = reader.Count(sizeof(std::uint64_t) * 3);
  if (!build_identity || !entry || !severity || !count ||
      std::get<0>(*severity) > static_cast<std::uint8_t>(frontend::DiagnosticSeverity::Error)) {
    return std::unexpected(reader.Error("invalid compilation cache header"));
  }
  CacheImage image{
      .build_identity = std::move(*build_identity),
      .entry = std::move(*entry),
      .options =
          {
              .implicit_dynamic_conversion =
                  static_cast<frontend::DiagnosticSeverity>(std::get<0>(*severity)),
          },
      .diagnostics = {},
      .inputs = {},
      .resolutions = {},
      .bytecode = {},
  };
  image.inputs.reserve(*count);
  for (std::size_t index = 0; index < *count; ++index) {
    auto path = reader.Text();
    const auto record = reader.Record<std::uint64_t, std::uint64_t>();
    if (!path || !record) {
      return std::unexpected(reader.Error("invalid compilation cache input"));
    }
    const auto [low, high] = *record;
    image.inputs.push_back({
        .path = std::move(*path),
        .fingerprint = {.low = low, .high = high},
    });
  }
  const auto resolutions = reader.Count(sizeof(std::uint64_t) * 2);
  if (!resolutions) {
    return std::unexpected(resolutions.error());
  }
  image.resolutions.reserve(*resolutions);
  for (std::size_t index = 0; index < *resolutions; ++index) {
    auto requested = reader.Text();
    auto canonical = reader.Text();
    if (!requested || !canonical) {
      return std::unexpected(reader.Error("invalid compilation cache resolution"));
    }
    image.resolutions.push_back(
        {.requested_path = std::move(*requested), .canonical_path = std::move(*canonical)});
  }
  const auto diagnostic_count = reader.Count((sizeof(std::uint32_t) * 3) +
                                             (sizeof(std::uint8_t) * 2) + sizeof(std::uint64_t));
  if (!diagnostic_count) {
    return std::unexpected(diagnostic_count.error());
  }
  for (std::size_t index = 0; index < *diagnostic_count; ++index) {
    const auto location =
        reader.Record<std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t, std::uint8_t>();
    auto message = reader.Text();
    if (!location || !message) {
      return std::unexpected(reader.Error("invalid cached diagnostic"));
    }
    const auto [source_id, line, column, code, kind] = *location;
    if (code > static_cast<std::uint8_t>(frontend::DiagnosticCode::ImplicitDynamicConversion) ||
        kind > static_cast<std::uint8_t>(frontend::DiagnosticKind::Internal) ||
        source_id >= image.inputs.size() || line == 0 || column == 0) {
      return std::unexpected(reader.Error("invalid cached diagnostic metadata"));
    }
    image.diagnostics.push_back({
        .location = {.line = line, .column = column, .source_id = source_id},
        .message = std::move(*message),
        .source_path = image.inputs.at(source_id).path,
        .kind = static_cast<frontend::DiagnosticKind>(kind),
        .severity = frontend::DiagnosticSeverity::Warning,
        .code = static_cast<frontend::DiagnosticCode>(code),
    });
  }
  const auto bytecode = reader.Take(reader.remaining());
  image.bytecode = *bytecode;
  return image;
}

std::expected<CacheImage, BinaryError> DecodeImage(std::span<const std::byte> bytes) {
  BinaryReader reader(bytes);
  const auto magic = reader.Take(kCacheMagic.size());
  if (!magic || !std::ranges::equal(*magic, std::as_bytes(std::span(kCacheMagic)))) {
    return std::unexpected(reader.Error("invalid compilation cache signature"));
  }
  const auto payload = reader.ChecksummedRemainder();
  if (!payload) {
    return std::unexpected(payload.error());
  }
  BinaryReader body(*payload);
  return ReadImage(body);
}

bool IsCurrent(const InputIdentity& input) {
  if (input.path.starts_with(frontend::kSdkSourcePrefix)) {
    const auto source = frontend::SdkModuleSource(input.path);
    return source && ContentFingerprint(*source) == input.fingerprint;
  }
  if (input.path.starts_with(frontend::kStandardSourcePrefix)) {
    const auto source = frontend::StandardDeclarationSource(
        input.path.substr(frontend::kStandardSourcePrefix.size()));
    return source && ContentFingerprint(*source) == input.fingerprint;
  }
  const auto path = Utf8Path(input.path);
  const auto bytes = ReadBinaryFile(path);
  return bytes && ContentFingerprint(*bytes) == input.fingerprint;
}

bool IsCurrent(const CacheImage& image,
               std::string_view entry,
               frontend::DiagnosticOptions options) {
  if (image.build_identity != kCompilerBuildIdentity || image.entry != entry ||
      image.options.implicit_dynamic_conversion != options.implicit_dynamic_conversion ||
      !std::ranges::all_of(image.inputs,
                           [](const auto& input) -> bool { return IsCurrent(input); })) {
    return false;
  }
  return std::ranges::all_of(image.resolutions, [](const auto& resolution) -> bool {
    std::error_code error;
    const auto canonical =
        std::filesystem::weakly_canonical(Utf8Path(resolution.requested_path), error);
    return !error && PathText(canonical) == resolution.canonical_path;
  });
}

void WriteInputs(BinaryWriter& writer, std::span<const frontend::SourceFile> inputs) {
  for (const auto& input : inputs) {
    writer.Write(input.path);
    const auto fingerprint = ContentFingerprint(input.text);
    writer.Write(fingerprint.low);
    writer.Write(fingerprint.high);
  }
}

BinaryWriter PrepareImage(const frontend::Program& source,
                          std::string_view entry,
                          frontend::DiagnosticOptions options) {
  BinaryWriter writer;
  writer.Write(kCompilerBuildIdentity);
  writer.Write(entry);
  writer.Write(static_cast<std::uint8_t>(options.implicit_dynamic_conversion));
  writer.Write(static_cast<std::uint64_t>(source.sources.size()));
  WriteInputs(writer, source.sources);
  writer.Write(static_cast<std::uint64_t>(source.file_resolutions.size()));
  for (const auto& resolution : source.file_resolutions) {
    writer.Write(resolution.requested_path);
    writer.Write(resolution.canonical_path);
  }
  return writer;
}

std::expected<CachedCompileResult, std::string> Rebuild(std::string_view path,
                                                        const std::filesystem::path& cache_path,
                                                        runtime::Isolate& isolate,
                                                        CacheStatus status,
                                                        frontend::DiagnosticOptions options) {
  auto parsed = frontend::LoadProgram(path);
  auto body = PrepareImage(parsed.program, path, options);
  auto compiled = CompileParsed(std::move(parsed), isolate, options);
  if (!compiled.Succeeded()) {
    return CachedCompileResult{.compiled = std::move(compiled), .status = status};
  }
  const auto bytecode = vm::SaveBytecode(compiled.program, isolate);
  if (!bytecode) {
    return std::unexpected(bytecode.error().message);
  }
  body.Write(static_cast<std::uint64_t>(compiled.diagnostics.size()));
  for (const auto& diagnostic : compiled.diagnostics) {
    body.Write(diagnostic.location.source_id);
    body.Write(diagnostic.location.line);
    body.Write(diagnostic.location.column);
    body.Write(static_cast<std::uint8_t>(diagnostic.code));
    body.Write(static_cast<std::uint8_t>(diagnostic.kind));
    body.Write(diagnostic.message);
  }
  body.Write(*bytecode);
  BinaryWriter image;
  image.Write(std::as_bytes(std::span(kCacheMagic)));
  image.WriteChecksummed(body.bytes());
  std::error_code error;
  std::filesystem::create_directories(cache_path.parent_path(), error);
  if (error) {
    return std::unexpected("cannot create compilation cache: " + error.message());
  }
  const auto written = WriteFileAtomically(cache_path, image.bytes());
  if (!written) {
    return std::unexpected(written.error());
  }
  return CachedCompileResult{.compiled = std::move(compiled), .status = status};
}

}  // namespace

std::expected<CachedCompileResult, std::string> CompileFileCached(
    std::string_view path,
    const std::filesystem::path& cache_directory,
    runtime::Isolate& isolate,
    frontend::DiagnosticOptions options) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(Utf8Path(path), error).lexically_normal();
  if (error) {
    return std::unexpected("cannot resolve cache entry: " + error.message());
  }
  const auto entry = PathText(absolute);
  const auto cache_path = cache_directory / (ContentFingerprint(entry).Hex() + ".vlcache");
  const auto exists = std::filesystem::exists(cache_path, error);
  if (error) {
    return std::unexpected("cannot inspect compilation cache: " + error.message());
  }
  if (!exists) {
    return Rebuild(entry, cache_path, isolate, CacheStatus::Absent, options);
  }
  const auto bytes = ReadBinaryFile(cache_path);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  const auto image = DecodeImage(*bytes);
  if (!image) {
    return Rebuild(entry, cache_path, isolate, CacheStatus::Invalid, options);
  }
  if (!IsCurrent(*image, entry, options)) {
    return Rebuild(entry, cache_path, isolate, CacheStatus::Stale, options);
  }
  auto loaded = vm::LoadBytecode(image->bytecode, isolate);
  if (!loaded) {
    return Rebuild(entry, cache_path, isolate, CacheStatus::Invalid, options);
  }
  return CachedCompileResult{
      .compiled = {.program = std::move(*loaded), .diagnostics = image->diagnostics},
      .status = CacheStatus::Hit,
  };
}

}  // namespace vela::compiler
