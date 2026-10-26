#include "vm/bytecode_image.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/binary.h"
#include "base/fingerprint.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "vm/bytecode_image_internal.h"
#include "vm/program.h"

namespace vela::vm {
namespace {

using internal::kNoImageId;
using internal::StandardClass;
using internal::WriteNumbers;

template <class T>
struct ImageTable final {
  std::vector<const T*> values;
  absl::flat_hash_map<const T*, std::uint64_t> ids;
  std::uint64_t Add(const T* value) {
    if (value == nullptr) {
      return kNoImageId;
    }
    const auto [entry, inserted] =
        ids.try_emplace(value, static_cast<std::uint64_t>(values.size()));
    if (inserted) {
      values.push_back(value);
    }
    return entry->second;
  }
  std::uint64_t Index(const T* value) const {
    return value == nullptr ? kNoImageId : ids.at(value);
  }
};

struct ImageWriter final {
  explicit ImageWriter(const runtime::Isolate& owner) : isolate(&owner) {}
  const runtime::Isolate* isolate;
  BinaryWriter writer;
  ImageTable<runtime::ClassLayout> classes;
  ImageTable<runtime::ArrayLayout> arrays;
  ImageTable<runtime::CollectionLayout> collections;
  ImageTable<LayoutTables> tables;
  ImageTable<StringConstants> strings;
  ImageTable<std::vector<std::string>> paths;

  void Collect(const Program& program);
  void AddElement(const runtime::ArrayLayout& element);
  void AddType(const runtime::TypeConstraint& type);
  void WriteElement(const runtime::ArrayLayout& element);
  void WriteType(const runtime::TypeConstraint& type);
  void WriteClass(const runtime::ClassLayout& layout);
  void WriteTables();
  void WriteFunction(const FunctionStorage& function);
  void WriteTexts(std::span<const std::string> values);
};

void ImageWriter::AddElement(const runtime::ArrayLayout& element) {
  classes.Add(element.element_class);
  arrays.Add(element.element_array);
  collections.Add(element.element_collection);
}

void ImageWriter::AddType(const runtime::TypeConstraint& type) {
  classes.Add(type.instance);
  arrays.Add(type.array);
  collections.Add(type.collection);
}

void ImageWriter::Collect(const Program& program) {
  for (const auto& function : program.functions()) {
    tables.Add(function.layouts.get());
    strings.Add(function.strings.get());
    paths.Add(function.source_paths.get());
  }
  for (const auto* table : tables.values) {
    for (const auto* layout : table->classes()) {
      classes.Add(layout);
    }
    for (const auto* layout : table->arrays()) {
      arrays.Add(layout);
    }
    for (const auto* layout : table->collections()) {
      collections.Add(layout);
    }
    for (const auto& type : table->types()) {
      AddType(type);
    }
  }
  std::size_t array = 0;
  std::size_t collection = 0;
  while (array < arrays.values.size() || collection < collections.values.size()) {
    if (array < arrays.values.size()) {
      AddElement(*arrays.values.at(array++));
    }
    if (collection < collections.values.size()) {
      const auto& layout = *collections.values.at(collection++);
      AddElement(layout.keys);
      if (layout.values) {
        AddElement(*layout.values);
      }
    }
  }
}

void ImageWriter::WriteElement(const runtime::ArrayLayout& element) {
  WriteType(element.element_type());
}

void ImageWriter::WriteType(const runtime::TypeConstraint& type) {
  writer.Write(static_cast<std::uint8_t>(type.kind));
  writer.Write(static_cast<std::uint8_t>(type.object_kind));
  const auto flags =
      static_cast<std::uint8_t>((type.nullable ? internal::kNullableImageFlag : 0) |
                                (type.callable ? internal::kCallableImageFlag : 0) |
                                (type.interface ? internal::kInterfaceImageFlag : 0));
  writer.Write(flags);
  writer.Write(type.interface.value_or(0));
  writer.Write(classes.Index(type.instance));
  writer.Write(arrays.Index(type.array));
  writer.Write(collections.Index(type.collection));
}

void ImageWriter::WriteClass(const runtime::ClassLayout& layout) {
  auto standard = StandardClass::None;
  if (&layout == &isolate->error_layout()) {
    standard = StandardClass::Error;
  } else if (&layout == &isolate->date_parts_layout()) {
    standard = StandardClass::DateParts;
  }
  writer.Write(static_cast<std::uint8_t>(standard));
  writer.Write(layout.field_count());
  WriteNumbers(writer, layout.reference_fields());
  writer.Write(static_cast<std::uint8_t>(layout.callable().has_value()));
  if (layout.callable()) {
    writer.Write(layout.callable()->function);
    writer.Write(static_cast<std::uint8_t>(layout.callable()->mode));
  }
  WriteNumbers(writer, layout.interfaces());
  WriteNumbers(writer, layout.dispatch_targets());
}

void ImageWriter::WriteTables() {
  writer.Write(static_cast<std::uint64_t>(classes.values.size()));
  for (const auto* layout : classes.values) {
    WriteClass(*layout);
  }
  writer.Write(static_cast<std::uint64_t>(arrays.values.size()));
  for (const auto* layout : arrays.values) {
    WriteElement(*layout);
  }
  writer.Write(static_cast<std::uint64_t>(collections.values.size()));
  for (const auto* layout : collections.values) {
    WriteElement(layout->keys);
    writer.Write(static_cast<std::uint8_t>(layout->values.has_value()));
    if (layout->values) {
      WriteElement(*layout->values);
    }
  }
  writer.Write(static_cast<std::uint64_t>(tables.values.size()));
  for (const auto* table : tables.values) {
    writer.Write(static_cast<std::uint64_t>(table->classes().size()));
    for (const auto* layout : table->classes()) {
      writer.Write(classes.Index(layout));
    }
    writer.Write(static_cast<std::uint64_t>(table->arrays().size()));
    for (const auto* layout : table->arrays()) {
      writer.Write(arrays.Index(layout));
    }
    writer.Write(static_cast<std::uint64_t>(table->collections().size()));
    for (const auto* layout : table->collections()) {
      writer.Write(collections.Index(layout));
    }
    writer.Write(static_cast<std::uint64_t>(table->types().size()));
    for (const auto& type : table->types()) {
      WriteType(type);
    }
  }
  writer.Write(static_cast<std::uint64_t>(strings.values.size()));
  for (const auto* pool : strings.values) {
    writer.Write(static_cast<std::uint64_t>(pool->values.size()));
    for (const auto* string : pool->values) {
      writer.Write(static_cast<std::uint8_t>(string != nullptr));
      if (string != nullptr) {
        writer.Write(string->value());
      }
    }
  }
  writer.Write(static_cast<std::uint64_t>(paths.values.size()));
  for (const auto* pool : paths.values) {
    WriteTexts(*pool);
  }
}

void ImageWriter::WriteTexts(std::span<const std::string> values) {
  writer.Write(static_cast<std::uint64_t>(values.size()));
  for (const auto& value : values) {
    writer.Write(std::string_view(value));
  }
}

void ImageWriter::WriteFunction(const FunctionStorage& function) {
  writer.Write(std::string_view(function.name));
  writer.Write(tables.Index(function.layouts.get()));
  writer.Write(strings.Index(function.strings.get()));
  writer.Write(paths.Index(function.source_paths.get()));
  writer.Write(function.register_count);
  writer.Write(function.parameter_count);
  writer.Write(static_cast<std::uint8_t>(function.is_async));
  WriteNumbers(writer, std::span(function.code));
  WriteNumbers(writer, std::span(function.integers));
  WriteNumbers(writer, std::span(function.doubles));
  writer.Write(static_cast<std::uint64_t>(function.source_mappings.size()));
  for (const auto& mapping : function.source_mappings) {
    writer.Write(mapping.instruction);
    writer.Write(mapping.location.line);
    writer.Write(mapping.location.column);
    writer.Write(mapping.location.source_id);
  }
  writer.Write(static_cast<std::uint64_t>(function.handlers.size()));
  for (const auto& handler : function.handlers) {
    writer.Write(handler.begin);
    writer.Write(handler.end);
    writer.Write(handler.target);
  }
  writer.Write(function.native != nullptr ? function.native->name() : std::string_view());
  if (function.native != nullptr) {
    writer.Write(static_cast<std::uint8_t>(function.native->result_type()));
    writer.Write(static_cast<std::uint64_t>(function.native->parameters().size()));
    for (const auto type : function.native->parameters()) {
      writer.Write(static_cast<std::uint8_t>(type));
    }
  }
}

}  // namespace

std::expected<std::vector<std::byte>, BinaryError> SaveBytecode(const Program& program,
                                                                const runtime::Isolate& isolate) {
  if (!program.IsForIsolate(isolate)) {
    return std::unexpected(
        BinaryError{.message = "cannot save a program from a different isolate"});
  }
  ImageWriter image(isolate);
  image.Collect(program);
  image.WriteTables();
  image.writer.Write(static_cast<std::uint64_t>(program.functions().size()));
  for (const auto& function : program.functions()) {
    image.WriteFunction(function);
  }
  const auto* globals = program.globals();
  WriteNumbers(image.writer, globals != nullptr
                                 ? std::span<const std::uint32_t>(globals->initializers)
                                 : std::span<const std::uint32_t>());
  BinaryWriter result;
  result.Write(
      std::as_bytes(std::span(internal::kImageMagic.data(), internal::kImageMagic.size())));
  result.Write(kBytecodeImageVersion);
  const auto fingerprint = ContentFingerprint(image.writer.bytes());
  result.Write(fingerprint.low);
  result.Write(fingerprint.high);
  result.Write(image.writer.bytes());
  return std::move(result).Take();
}

}  // namespace vela::vm
