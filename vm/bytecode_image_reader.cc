#include "vm/bytecode_image.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "base/binary.h"
#include "base/fingerprint.h"
#include "runtime/isolate.h"
#include "runtime/layout_transaction.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "vm/bytecode_image_internal.h"
#include "vm/program.h"

namespace vela::vm {
namespace {

using internal::kNoImageId;
using internal::ReadNumbers;

template <class T>
std::expected<std::vector<T*>, BinaryError> ReadReferences(BinaryReader& reader,
                                                           std::span<T* const> values) {
  const auto indices = ReadNumbers<std::uint64_t>(reader);
  if (!indices) {
    return std::unexpected(indices.error());
  }
  std::vector<T*> result;
  result.reserve(indices->size());
  for (const auto index : *indices) {
    if (index == kNoImageId) {
      result.push_back(nullptr);
    } else if (index >= values.size()) {
      return std::unexpected(reader.Error("invalid layout-table index"));
    } else {
      result.push_back(values.subspan(static_cast<std::size_t>(index)).front());
    }
  }
  return result;
}

std::expected<std::vector<std::shared_ptr<const LayoutTables>>, BinaryError> ReadTables(
    BinaryReader& reader,
    const internal::LoadedLayouts& layouts) {
  const auto count = reader.Count(4 * sizeof(std::uint64_t));
  if (!count) {
    return std::unexpected(count.error());
  }
  std::vector<std::shared_ptr<const LayoutTables>> tables;
  for (std::size_t index = 0; index < *count; ++index) {
    auto classes = ReadReferences(reader, std::span(layouts.classes));
    auto arrays = ReadReferences(reader, std::span(layouts.arrays));
    auto collections = ReadReferences(reader, std::span(layouts.collections));
    if (!classes) {
      return std::unexpected(classes.error());
    }
    if (!arrays) {
      return std::unexpected(arrays.error());
    }
    if (!collections) {
      return std::unexpected(collections.error());
    }
    const auto type_count = reader.Count(internal::kElementBytes);
    if (!type_count) {
      return std::unexpected(type_count.error());
    }
    std::vector<runtime::TypeConstraint> types;
    for (std::size_t type = 0; type < *type_count; ++type) {
      const auto input = internal::ReadElement(reader);
      if (!input) {
        return std::unexpected(input.error());
      }
      const auto resolved = internal::ResolveType(*input, layouts);
      if (!resolved) {
        return std::unexpected(reader.Error(resolved.error()));
      }
      types.push_back(*resolved);
    }
    tables.push_back(std::make_shared<const LayoutTables>(
        std::move(*classes), std::move(*arrays), std::move(*collections), std::move(types)));
  }
  return tables;
}

std::expected<std::vector<std::shared_ptr<const StringConstants>>, BinaryError> ReadStrings(
    BinaryReader& reader,
    runtime::Isolate& isolate) {
  const auto count = reader.Count(sizeof(std::uint64_t));
  if (!count) {
    return std::unexpected(count.error());
  }
  std::vector<std::shared_ptr<const StringConstants>> pools;
  for (std::size_t index = 0; index < *count; ++index) {
    const auto strings = reader.Count(1);
    if (!strings) {
      return std::unexpected(strings.error());
    }
    auto pool = std::make_shared<StringConstants>(isolate.heap(), *strings);
    for (std::size_t string = 0; string < *strings; ++string) {
      const auto present = reader.Read<std::uint8_t>();
      if (!present) {
        return std::unexpected(present.error());
      }
      if (*present > 1) {
        return std::unexpected(reader.Error("invalid string-constant flag"));
      }
      if (*present == 0) {
        continue;
      }
      const auto text = reader.Text();
      if (!text) {
        return std::unexpected(text.error());
      }
      auto* value = isolate.heap().AllocateString(*text);
      if (value == nullptr) {
        return std::unexpected(reader.Error("out of memory loading string constants"));
      }
      pool->values.at(string) = value;
    }
    pools.push_back(std::move(pool));
  }
  return pools;
}

std::expected<std::vector<std::shared_ptr<const std::vector<std::string>>>, BinaryError> ReadPaths(
    BinaryReader& reader) {
  const auto count = reader.Count(sizeof(std::uint64_t));
  if (!count) {
    return std::unexpected(count.error());
  }
  std::vector<std::shared_ptr<const std::vector<std::string>>> pools;
  for (std::size_t index = 0; index < *count; ++index) {
    auto paths = internal::ReadTexts(reader);
    if (!paths) {
      return std::unexpected(paths.error());
    }
    pools.push_back(std::make_shared<const std::vector<std::string>>(std::move(*paths)));
  }
  return pools;
}

std::expected<void, BinaryError> ReadDebug(BinaryReader& reader, FunctionStorage& function) {
  const auto mappings = reader.Count(4 * sizeof(std::uint32_t));
  if (!mappings) {
    return std::unexpected(mappings.error());
  }
  for (std::size_t index = 0; index < *mappings; ++index) {
    const auto mapping =
        reader.Record<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>();
    if (!mapping) {
      return std::unexpected(mapping.error());
    }
    const auto [instruction, line, column, source_id] = *mapping;
    function.source_mappings.push_back({
        .instruction = instruction,
        .location = {.line = line, .column = column, .source_id = source_id},
    });
  }
  const auto handlers = reader.Count(3 * sizeof(std::uint32_t));
  if (!handlers) {
    return std::unexpected(handlers.error());
  }
  for (std::size_t index = 0; index < *handlers; ++index) {
    const auto handler = reader.Record<std::uint32_t, std::uint32_t, std::uint32_t>();
    if (!handler) {
      return std::unexpected(handler.error());
    }
    const auto [begin, end, target] = *handler;
    function.handlers.push_back({.begin = begin, .end = end, .target = target});
  }
  return {};
}

std::expected<std::shared_ptr<const runtime::NativeFunction>, BinaryError> ReadNative(
    BinaryReader& reader,
    const runtime::Isolate& isolate) {
  const auto symbol = reader.Text();
  if (!symbol) {
    return std::unexpected(symbol.error());
  }
  if (symbol->empty()) {
    return nullptr;
  }
  const auto result_type = reader.Read<std::uint8_t>();
  const auto parameters = ReadNumbers<std::uint8_t>(reader);
  if (!result_type) {
    return std::unexpected(result_type.error());
  }
  if (!parameters) {
    return std::unexpected(parameters.error());
  }
  auto native = isolate.FindNative(*symbol);
  if (native == nullptr) {
    return std::unexpected(reader.Error("unregistered native symbol '" + *symbol + "'"));
  }
  if (*result_type != static_cast<std::uint8_t>(native->result_type()) ||
      !std::ranges::equal(*parameters, native->parameters(), {}, {}, [](auto type) -> std::uint8_t {
        return static_cast<std::uint8_t>(type);
      })) {
    return std::unexpected(reader.Error("native signature mismatch for '" + *symbol + "'"));
  }
  return native;
}

template <class T>
std::expected<std::shared_ptr<const T>, BinaryError>
Pool(std::uint64_t index, std::span<const std::shared_ptr<const T>> pools, BinaryReader& reader) {
  if (index == kNoImageId) {
    return nullptr;
  }
  if (index >= pools.size()) {
    return std::unexpected(reader.Error("invalid function pool index"));
  }
  return pools.subspan(static_cast<std::size_t>(index)).front();
}

struct ImageReader final {
  BinaryReader* reader = nullptr;
  const runtime::Isolate* isolate = nullptr;
  std::span<const std::shared_ptr<const LayoutTables>> tables;
  std::span<const std::shared_ptr<const StringConstants>> strings;
  std::span<const std::shared_ptr<const std::vector<std::string>>> paths;
  std::expected<FunctionStorage, BinaryError> ReadFunction() const;
};

std::expected<FunctionStorage, BinaryError> ImageReader::ReadFunction() const {
  FunctionStorage function;
  auto name = reader->Text();
  if (!name) {
    return std::unexpected(name.error());
  }
  function.name = std::move(*name);
  const auto pools = reader->Record<std::uint64_t, std::uint64_t, std::uint64_t, std::uint16_t,
                                    std::uint16_t, std::uint8_t>();
  if (!pools) {
    return std::unexpected(pools.error());
  }
  const auto [table_id, strings_id, paths_id, registers, parameters, async] = *pools;
  auto table = Pool(table_id, tables, *reader);
  auto string_pool = Pool(strings_id, strings, *reader);
  auto path_pool = Pool(paths_id, paths, *reader);
  if (!table) {
    return std::unexpected(table.error());
  }
  if (!string_pool) {
    return std::unexpected(string_pool.error());
  }
  if (!path_pool) {
    return std::unexpected(path_pool.error());
  }
  function.layouts = std::move(*table);
  function.strings = std::move(*string_pool);
  function.source_paths = std::move(*path_pool);
  function.register_count = registers;
  function.parameter_count = parameters;
  if (async > 1) {
    return std::unexpected(reader->Error("invalid async function flag"));
  }
  function.is_async = async != 0;
  auto code = ReadNumbers<std::uint32_t>(*reader);
  auto integers = ReadNumbers<std::int64_t>(*reader);
  auto doubles = ReadNumbers<double>(*reader);
  if (!code) {
    return std::unexpected(code.error());
  }
  if (!integers) {
    return std::unexpected(integers.error());
  }
  if (!doubles) {
    return std::unexpected(doubles.error());
  }
  function.code = std::move(*code);
  function.integers = std::move(*integers);
  function.doubles = std::move(*doubles);
  if (auto debug = ReadDebug(*reader, function); !debug) {
    return std::unexpected(debug.error());
  }
  auto native = ReadNative(*reader, *isolate);
  if (!native) {
    return std::unexpected(native.error());
  }
  function.native = std::move(*native);
  return function;
}

std::expected<Program, BinaryError> ReadPayload(BinaryReader& reader, runtime::Isolate& isolate) {
  const auto layouts = internal::ReadLayouts(reader, isolate);
  if (!layouts) {
    return std::unexpected(layouts.error());
  }
  const auto tables = ReadTables(reader, *layouts);
  if (!tables) {
    return std::unexpected(tables.error());
  }
  const auto strings = ReadStrings(reader, isolate);
  if (!strings) {
    return std::unexpected(strings.error());
  }
  const auto paths = ReadPaths(reader);
  if (!paths) {
    return std::unexpected(paths.error());
  }
  const auto count = reader.Count((10 * sizeof(std::uint64_t)) + (2 * sizeof(std::uint16_t)) +
                                  sizeof(std::uint8_t));
  if (!count) {
    return std::unexpected(count.error());
  }
  const ImageReader image{
      .reader = &reader,
      .isolate = &isolate,
      .tables = *tables,
      .strings = *strings,
      .paths = *paths,
  };
  std::vector<FunctionStorage> functions;
  for (std::size_t index = 0; index < *count; ++index) {
    auto function = image.ReadFunction();
    if (!function) {
      return std::unexpected(function.error());
    }
    functions.push_back(std::move(*function));
  }
  auto initializers = ReadNumbers<std::uint32_t>(reader);
  if (!initializers) {
    return std::unexpected(initializers.error());
  }
  if (reader.remaining() != 0) {
    return std::unexpected(reader.Error("trailing bytecode image data"));
  }
  auto program = Program::Create(std::move(functions), isolate, std::move(*initializers));
  if (!program) {
    const auto& error = program.error();
    return std::unexpected(reader.Error("function " + std::to_string(error.function) +
                                        ", instruction " + std::to_string(error.instruction) +
                                        ": " + error.message));
  }
  return std::move(*program);
}

}  // namespace

std::expected<Program, BinaryError> LoadBytecode(std::span<const std::byte> bytes,
                                                 runtime::Isolate& isolate) {
  BinaryReader reader(bytes);
  const auto magic = reader.Take(internal::kImageMagic.size());
  if (!magic) {
    return std::unexpected(magic.error());
  }
  if (!std::ranges::equal(*magic, std::as_bytes(std::span(internal::kImageMagic.data(),
                                                          internal::kImageMagic.size())))) {
    return std::unexpected(reader.Error("invalid bytecode image magic"));
  }
  const auto header = reader.Record<std::uint32_t, std::uint64_t, std::uint64_t>();
  if (!header) {
    return std::unexpected(header.error());
  }
  const auto [version, low, high] = *header;
  if (version != kBytecodeImageVersion) {
    return std::unexpected(reader.Error("unsupported bytecode image version"));
  }
  const auto payload = reader.Take(reader.remaining());
  if (!payload) {
    return std::unexpected(payload.error());
  }
  if (ContentFingerprint(*payload) != Fingerprint{.low = low, .high = high}) {
    return std::unexpected(reader.Error("bytecode image checksum mismatch"));
  }
  BinaryReader body(*payload);
  runtime::LayoutTransaction transaction(isolate);
  auto program = ReadPayload(body, isolate);
  if (program) {
    transaction.Commit();
  }
  return program;
}

}  // namespace vela::vm
