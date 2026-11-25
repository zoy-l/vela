#include "compiler/native/native_image_data.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/binary.h"
#include "base/binary_codec.h"
#include "base/binary_fields.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::native {
namespace {

using namespace runtime;

using binary::Fields;

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeTypeDescription>
auto Fields(Record& value) {
  return std::tie(value.kind, value.nullable, value.index);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeFieldDescription>
auto Fields(Record& value) {
  return std::tie(value.declaration, value.type, value.flags, value.initializer);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeClassDescription>
auto Fields(Record& value) {
  return std::tie(value.name, value.native_key, value.native_contract, value.reference_fields,
                  value.interfaces, value.dispatch_targets, value.first_selector, value.field_count,
                  value.kind, value.callable_function, value.callable_mode, value.callable_type,
                  value.declaration, value.fields, value.base_class, value.constructor,
                  value.enum_members);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeArrayDescription>
auto Fields(Record& value) {
  return std::tie(value.name, value.element, value.record_fields);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeCollectionDescription>
auto Fields(Record& value) {
  return std::tie(value.name, value.key, value.value, value.has_value);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeSignatureParameter>
auto Fields(Record& value) {
  return std::tie(value.type, value.flags);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeSignatureDescription>
auto Fields(Record& value) {
  return std::tie(value.name, value.parameters, value.result, value.has_result);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeCallParameter>
auto Fields(Record& value) {
  return std::tie(value.name, value.type, value.flags);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeConstructionDescription>
auto Fields(Record& value) {
  return std::tie(value.type, value.supplied);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeBindingDependency>
auto Fields(Record& value) {
  return std::tie(value.target, value.receiver);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeDependencyBlock>
auto Fields(Record& value) {
  return std::tie(value.begin, value.end, value.successors);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeFunctionDependencies>
auto Fields(Record& value) {
  return std::tie(value.fields, value.globals, value.versions, value.constructions,
                  value.lexical_class, value.bindings, value.types, value.positions, value.blocks);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeFunctionDescription>
auto Fields(Record& value) {
  return std::tie(value.name, value.native_symbol, value.parameters, value.locations,
                  value.scalar_operand, value.parameter_count, value.result_type,
                  value.hidden_parameters, value.receiver_type, value.flags, value.scalar_capture,
                  value.identity, value.dependencies);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeMemberDescription>
auto Fields(Record& value) {
  return std::tie(value.name, value.receiver_type, value.target, value.type, value.setter,
                  value.kind);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeDeclarationDescription>
auto Fields(Record& value) {
  return std::tie(value.identity, value.owner, value.kind, value.name, value.type_parameters,
                  value.enum_members, value.generic_signature, value.definition);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeJSONDecodeParameter>
auto Fields(Record& value) {
  return std::tie(value.name, value.node, value.optional);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeJSONDecodePlan>
auto Fields(Record& value) {
  return std::tie(value.name, value.parameters, value.kind, value.type, value.element,
                  value.constructor);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeReflectionType>
auto Fields(Record& value) {
  return std::tie(value.type, value.module, value.name, value.arguments, value.base,
                  value.interfaces, value.flags);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeReflectionMember>
auto Fields(Record& value) {
  return std::tie(value.declaration, value.declaring_type, value.name, value.kind, value.flags,
                  value.type, value.target, value.getter, value.setter, value.parameters,
                  value.type_arguments);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeAttributeValue>
auto Fields(Record& value) {
  return std::tie(value.type, value.kind, value.payload, value.text);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeAttribute>
auto Fields(Record& value) {
  return std::tie(value.target, value.type, value.fields);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, NativeImage>
auto Fields(Record& value) {
  return std::tie(
      value.abi_version, value.record_bytes, value.abi_identity, value.target, value.interfaces,
      value.classes, value.arrays, value.collections, value.signatures, value.types,
      value.array_types, value.collection_types, value.strings, value.source_paths, value.functions,
      value.members, value.global_initializers, value.revision, value.source_texts,
      value.declarations, value.interface_declarations, value.global_declarations,
      value.selector_declarations, value.native_mode, value.global_flags, value.json_decode_plans,
      value.reflection_types, value.reflection_members, value.attributes, value.imported_functions);
}

struct ImageRecords final {
  template <class T>
  static auto Record(T& value) -> decltype(Fields(value)) {
    return Fields(value);
  }
};
using NativeImageEncoder = BinaryEncoder<ImageRecords>;

}  // namespace

class NativeImageDecoder final : public BinaryDecoder<NativeImageDecoder, ImageRecords> {
 public:
  explicit NativeImageDecoder(std::span<const std::byte> bytes) : BinaryDecoder(bytes) {}
  std::expected<std::shared_ptr<NativeImageData>, std::string> Decode() {
    Read(data_->image_);
    if (auto finished = Finish(); !finished) {
      return std::unexpected(std::move(finished.error()));
    }
    return data_;
  }

 private:
  template <class T>
  T* Allocate(std::size_t count) {
    if (!CanAllocate(count, sizeof(T))) {
      return nullptr;
    }
    if constexpr (std::is_same_v<T, runtime::NativeFunctionDescription>) {
      data_->functions_.resize(count);
      return data_->functions_.data();
    } else {
      auto* memory = static_cast<T*>(data_->storage_.Allocate(count * sizeof(T), alignof(T)));
      std::uninitialized_value_construct_n(memory, count);
      return memory;
    }
  }

 public:
  void ReadCustom(runtime::NativeText& value) {
    auto text = reader().Text();
    if (!text) {
      error() = text.error().message;
      return;
    }
    const auto found = texts_.find(*text);
    if (found != texts_.end()) {
      value = found->second;
      return;
    }
    auto* memory = Allocate<char>(text->size());
    if (memory == nullptr) {
      return;
    }
    std::ranges::copy(*text, memory);
    value = {.data = memory, .size = text->size()};
    texts_.emplace(value.text(), value);
  }
  template <class T>
    requires std::is_pointer_v<T>
  void ReadCustom(T& value) {
    bool present{};
    Read(present);
    if (!present) {
      return;
    }
    using Element = std::remove_const_t<std::remove_pointer_t<T>>;
    auto* memory = Allocate<Element>(1);
    if (!memory) {
      return;
    }
    Read(*memory);
    value = memory;
  }
  template <class T>
    requires requires(T value) { value.values(); }
  void ReadCustom(T& value) {
    const auto count = reader().Count(1);
    if (!count) {
      error() = count.error().message;
      return;
    }
    if (*count == 0) {
      return;
    }
    using Element = std::remove_const_t<std::remove_pointer_t<decltype(value.data)>>;
    auto* memory = Allocate<Element>(*count);
    if (!memory) {
      return;
    }
    value = {memory, *count};
    for (auto& item : std::span(memory, *count)) {
      Read(item);
    }
  }

 private:
  std::shared_ptr<NativeImageData> data_ = std::make_shared<NativeImageData>();
  absl::flat_hash_map<std::string_view, runtime::NativeText> texts_;
};

std::vector<std::byte> EncodeNativeImage(const runtime::NativeImage& image) {
  BinaryWriter writer;
  NativeImageEncoder encoder(writer);
  encoder.Write(image);
  return std::move(writer).Take();
}
std::expected<std::shared_ptr<NativeImageData>, std::string> DecodeNativeImage(
    std::span<const std::byte> bytes) {
  return NativeImageDecoder(bytes).Decode();
}

}  // namespace vela::compiler::native
