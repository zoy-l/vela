#include "base/callable.h"
#include "base/member.h"
#include "vm/bytecode_image.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/binary.h"
#include "runtime/isolate.h"
#include "runtime/layout_transaction.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "vm/bytecode_image_internal.h"
#include "vm/debug_info.h"
#include "vm/exception_handler.h"
#include "vm/program.h"

namespace vela::vm {
namespace {

using internal::kNoImageId;
using internal::ReadNumbers;

constexpr std::size_t kMinimumTableBytes = 5 * sizeof(std::uint64_t);
constexpr std::size_t kMinimumFunctionBytes =
    (10 * sizeof(std::uint64_t)) + (2 * sizeof(std::uint16_t)) + sizeof(std::uint8_t);

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
    const auto value = internal::ResolvePointer(index, values);
    if (!value) {
      return std::unexpected(reader.Error(value.error()));
    }
    result.push_back(*value);
  }
  return result;
}

std::expected<runtime::TypeConstraint, BinaryError> ReadType(
    BinaryReader& reader,
    const internal::LoadedLayouts& layouts) {
  const auto input = internal::ReadElement(reader);
  if (!input) {
    return std::unexpected(input.error());
  }
  const auto type = internal::ResolveType(*input, layouts);
  if (!type) {
    return std::unexpected(reader.Error(type.error()));
  }
  return *type;
}

std::expected<MemberDefinition, BinaryError> ReadMember(BinaryReader& reader) {
  const auto receiver = reader.Read<std::uint32_t>();
  auto name = reader.Text();
  const auto record = reader.Record<std::uint8_t, std::uint32_t, std::uint32_t, std::uint32_t>();
  if (!receiver) {
    return std::unexpected(receiver.error());
  }
  if (!name) {
    return std::unexpected(name.error());
  }
  if (!record) {
    return std::unexpected(record.error());
  }
  const auto [kind, target, type, setter] = *record;
  if (kind > static_cast<std::uint8_t>(MemberKind::Property)) {
    return std::unexpected(reader.Error("invalid member kind"));
  }
  return MemberDefinition{
      .receiver_type = *receiver,
      .name = std::move(*name),
      .kind = static_cast<MemberKind>(kind),
      .target = target,
      .type = type == kInvalidFunctionTarget ? std::nullopt : std::optional(type),
      .setter = setter == kInvalidFunctionTarget ? std::nullopt : std::optional(setter),
  };
}

std::expected<CallSignature::Parameter, BinaryError> ReadCallParameter(BinaryReader& reader) {
  auto name = reader.Text();
  if (!name) {
    return std::unexpected(name.error());
  }
  const auto type = reader.Read<std::uint32_t>();
  if (!type) {
    return std::unexpected(type.error());
  }
  const auto flags = internal::ReadParameterFlags(reader);
  if (!flags) {
    return std::unexpected(flags.error());
  }
  return CallSignature::Parameter{
      .name = std::move(*name),
      .type = *type,
      .optional = flags->optional,
      .rest = flags->rest,
  };
}

std::expected<std::optional<CallSignature>, BinaryError> ReadCallSignature(BinaryReader& reader) {
  const auto present = internal::ReadFlag(reader);
  if (!present) {
    return std::unexpected(present.error());
  }
  if (!*present) {
    return std::optional<CallSignature>{};
  }
  const auto header = reader.Record<std::uint8_t, std::uint32_t>();
  if (!header) {
    return std::unexpected(header.error());
  }
  constexpr auto kParameterBytes = sizeof(std::uint64_t) + sizeof(std::uint32_t) + 2;
  auto parameters =
      internal::ReadRecords<CallSignature::Parameter>(reader, kParameterBytes, ReadCallParameter);
  if (!parameters) {
    return std::unexpected(parameters.error());
  }
  const auto [hidden, result] = *header;
  CallSignature signature{
      .parameters = std::move(*parameters),
      .hidden_parameters = hidden,
      .parameter_name_order = {},
  };
  if (result != kInvalidFunctionTarget) {
    signature.result = result;
  }
  return std::optional(std::move(signature));
}

std::expected<std::vector<std::shared_ptr<const LayoutTables>>, BinaryError> ReadTables(
    BinaryReader& input,
    const internal::LoadedLayouts& layouts) {
  return internal::ReadRecords<std::shared_ptr<const LayoutTables>>(
      input, kMinimumTableBytes,
      [&layouts](
          BinaryReader& reader) -> std::expected<std::shared_ptr<const LayoutTables>, BinaryError> {
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
        auto types = internal::ReadRecords<runtime::TypeConstraint>(
            reader, internal::kElementBytes,
            [&layouts](BinaryReader& type_input) { return ReadType(type_input, layouts); });
        if (!types) {
          return std::unexpected(types.error());
        }
        constexpr auto kMemberBytes = sizeof(std::uint64_t) + (4 * sizeof(std::uint32_t)) + 1;
        auto members = internal::ReadRecords<MemberDefinition>(reader, kMemberBytes, ReadMember);
        if (!members) {
          return std::unexpected(members.error());
        }
        return std::make_shared<const LayoutTables>(std::move(*classes), std::move(*arrays),
                                                    std::move(*collections), std::move(*types),
                                                    std::move(*members));
      });
}

std::expected<std::vector<std::shared_ptr<const StringConstants>>, BinaryError> ReadStrings(
    BinaryReader& input,
    runtime::Isolate& isolate) {
  return internal::ReadRecords<std::shared_ptr<const StringConstants>>(
      input, sizeof(std::uint64_t),
      [&isolate](BinaryReader& reader)
          -> std::expected<std::shared_ptr<const StringConstants>, BinaryError> {
        const auto strings = reader.Count(1);
        if (!strings) {
          return std::unexpected(strings.error());
        }
        auto pool = std::make_shared<StringConstants>(isolate.heap(), *strings);
        for (std::size_t string = 0; string < *strings; ++string) {
          const auto present = internal::ReadFlag(reader);
          if (!present) {
            return std::unexpected(present.error());
          }
          if (!*present) {
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
        return pool;
      });
}

std::expected<std::vector<std::shared_ptr<const std::vector<std::string>>>, BinaryError> ReadPaths(
    BinaryReader& input) {
  return internal::ReadRecords<std::shared_ptr<const std::vector<std::string>>>(
      input, sizeof(std::uint64_t),
      [](BinaryReader& reader)
          -> std::expected<std::shared_ptr<const std::vector<std::string>>, BinaryError> {
        auto paths = internal::ReadTexts(reader);
        if (!paths) {
          return std::unexpected(paths.error());
        }
        return std::make_shared<const std::vector<std::string>>(std::move(*paths));
      });
}

std::expected<void, BinaryError> ReadDebug(BinaryReader& reader, FunctionStorage& function) {
  auto mappings = internal::ReadRecords<SourceMapping>(
      reader, 4 * sizeof(std::uint32_t),
      [](BinaryReader& input) -> std::expected<SourceMapping, BinaryError> {
        const auto mapping =
            input.Record<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>();
        if (!mapping) {
          return std::unexpected(mapping.error());
        }
        const auto [instruction, line, column, source_id] = *mapping;
        return SourceMapping{
            .instruction = instruction,
            .location = {.line = line, .column = column, .source_id = source_id},
        };
      });
  if (!mappings) {
    return std::unexpected(mappings.error());
  }
  auto handlers = internal::ReadRecords<ExceptionHandler>(
      reader, 3 * sizeof(std::uint32_t),
      [](BinaryReader& input) -> std::expected<ExceptionHandler, BinaryError> {
        const auto handler = input.Record<std::uint32_t, std::uint32_t, std::uint32_t>();
        if (!handler) {
          return std::unexpected(handler.error());
        }
        const auto [begin, end, target] = *handler;
        return ExceptionHandler{.begin = begin, .end = end, .target = target};
      });
  if (!handlers) {
    return std::unexpected(handlers.error());
  }
  function.source_mappings = std::move(*mappings);
  function.handlers = std::move(*handlers);
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
  [[nodiscard]] std::expected<FunctionStorage, BinaryError> ReadFunction() const;
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
  auto signature = ReadCallSignature(*reader);
  if (!signature) {
    return std::unexpected(signature.error());
  }
  function.signature = std::move(*signature);
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
  const ImageReader image{
      .reader = &reader,
      .isolate = &isolate,
      .tables = *tables,
      .strings = *strings,
      .paths = *paths,
  };
  auto functions = internal::ReadRecords<FunctionStorage>(
      reader, kMinimumFunctionBytes, [&image](BinaryReader&) { return image.ReadFunction(); });
  if (!functions) {
    return std::unexpected(functions.error());
  }
  auto initializers = ReadNumbers<std::uint32_t>(reader);
  if (!initializers) {
    return std::unexpected(initializers.error());
  }
  if (reader.remaining() != 0) {
    return std::unexpected(reader.Error("trailing bytecode image data"));
  }
  auto program = Program::Create(std::move(*functions), isolate, std::move(*initializers));
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
  const auto version = reader.Read<std::uint32_t>();
  if (!version) {
    return std::unexpected(version.error());
  }
  if (*version != kBytecodeImageVersion) {
    return std::unexpected(reader.Error("unsupported bytecode image version"));
  }
  const auto payload = reader.ChecksummedRemainder();
  if (!payload) {
    return std::unexpected(payload.error());
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
