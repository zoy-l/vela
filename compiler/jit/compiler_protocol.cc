#include "compiler/jit/compiler_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/fingerprint.h"
#include "base/inline_layout.h"
#include "base/json_decode.h"
#include "base/layout_description.h"
#include "base/member.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "base/reflection.h"
#include "base/type_kind.h"
#include "compiler/development/development_changes.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"
#include "compiler/jit/code_inputs.h"
#include "compiler/jit/function_compiler.h"
#include "compiler/native/function_symbols.h"

#ifndef _WIN32
#include <pthread.h>

// POSIX 信号集 API 由 <signal.h> 声明。
extern "C" {
#include <signal.h>
}
#ifdef __APPLE__
#include <sys/signal.h>
#endif
#endif

#include "base/binary.h"
#include "base/binary_codec.h"
#include "base/binary_fields.h"
#include "compiler/frontend/typing/type_name.h"

namespace vela::compiler::jit {
namespace {

// 成员顺序是 SDK 内部 IPC 契约；枚举、数字和长度都显式编码。
using binary::Fields;

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, CallableTarget>
auto Fields(Record& value) {
  return std::tie(value.function, value.mode);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, CallSignature::Parameter>
auto Fields(Record& value) {
  return std::tie(value.name, value.type, value.optional, value.rest, value.has_default);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, CallSignature>
auto Fields(Record& value) {
  return std::tie(value.parameters, value.result, value.hidden_parameters, value.receiver,
                  value.parameter_name_order, value.narrowed_parameter,
                  value.false_narrowed_parameter);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, MemberDefinition>
auto Fields(Record& value) {
  return std::tie(value.receiver_type, value.name, value.kind, value.target, value.type,
                  value.setter, value.is_read_only);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, frontend::TypeDescriptor>
auto Fields(Record& value) {
  return std::tie(value.kind, value.name, value.nullable, value.is_abstract, value.known_non_null,
                  value.value_layout);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, frontend::StructLayout>
auto Fields(Record& value) {
  return std::tie(value.fields, value.offsets, value.slots);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, std::shared_ptr<frontend::StructLayout>>
auto Fields(Record& value) {
  return std::tuple(value.get());
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, std::shared_ptr<const ir::StructReceiver>>
auto Fields(Record& value) {
  return std::tuple(value.get());
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, layout::Type>
auto Fields(Record& value) {
  return std::tie(value.kind, value.nullable, value.index);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, layout::Array>
auto Fields(Record& value) {
  return std::tie(value.name, value.element);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, layout::Collection>
auto Fields(Record& value) {
  return std::tie(value.name, value.key, value.value);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, layout::Signature::Parameter>
auto Fields(Record& value) {
  return std::tie(value.type, value.optional, value.rest);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, layout::Signature>
auto Fields(Record& value) {
  return std::tie(value.name, value.parameters, value.result, value.narrowed_parameter,
                  value.false_narrowed_parameter);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, layout::Class>
auto Fields(Record& value) {
  return std::tie(value.name, value.native_key, value.native_contract, value.kind,
                  value.field_count, value.reference_fields, value.callable, value.interfaces,
                  value.callable_type, value.value_slots);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, layout::Nominal>
auto Fields(Record& value) {
  return std::tie(value.module, value.name, value.arguments, value.exported);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, layout::Program>
auto Fields(Record& value) {
  return std::tie(value.interfaces, value.classes, value.arrays, value.collections,
                  value.signatures, value.types, value.array_types, value.collection_types,
                  value.nominals);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, ir::Construction>
auto Fields(Record& value) {
  return std::tie(value.type, value.supplied, value.generated);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, ir::StructReceiver>
auto Fields(Record& value) {
  return std::tie(value.kind, value.receiver, value.index, value.slot, value.declaration,
                  value.type, value.fields);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, ir::Instruction>
auto Fields(Record& value) {
  return std::tie(value.opcode, value.dst, value.lhs, value.rhs, value.immediate, value.index,
                  value.location, value.handler, value.array_type, value.field_declaration,
                  value.binding_receiver, value.reflection_member, value.construction,
                  value.completes_construction, value.inline_value, value.struct_receiver,
                  value.no_normal_return);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, ir::Class::Field>
auto Fields(Record& value) {
  return std::tie(value.declaration, value.type, value.visibility, value.binding,
                  value.initializer);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, ir::Class>
auto Fields(Record& value) {
  return std::tie(value.name, value.field_types, value.callable, value.interfaces,
                  value.dispatch_targets, value.kind, value.native_key, value.native_contract,
                  value.callable_type, value.declaration, value.fields, value.base_class,
                  value.constructor, value.enum_members, value.value_layout);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, ir::Function::Handler>
auto Fields(Record& value) {
  return std::tie(value.target);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, ir::Function>
auto Fields(Record& value) {
  return std::tie(value.name, value.location, value.return_type, value.parameter_count,
                  value.register_count, value.value_types, value.instructions, value.doubles,
                  value.native_symbol, value.handlers, value.is_async, value.is_exported,
                  value.is_generated_constructor, value.visibility, value.signature, value.identity,
                  value.type_dependencies, value.json_decode_plan, value.struct_receiver);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, ir::Declaration>
auto Fields(Record& value) {
  return std::tie(value.identity, value.owner, value.kind, value.name, value.type_parameters,
                  value.enum_members, value.generic_signature, value.definition);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, ir::JSONDecodeParameter>
auto Fields(Record& value) {
  return std::tie(value.name, value.node, value.optional);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, ir::JSONDecodePlan>
auto Fields(Record& value) {
  return std::tie(value.name, value.kind, value.type, value.element, value.constructor,
                  value.parameters);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, reflection::Type>
auto Fields(Record& value) {
  return std::tie(value.declaration, value.type, value.module, value.name, value.arguments,
                  value.base, value.interfaces, value.flags);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, reflection::Member>
auto Fields(Record& value) {
  return std::tie(value.declaration, value.declaring_type, value.name, value.kind, value.flags,
                  value.type, value.target, value.getter, value.setter, value.signature,
                  value.type_arguments);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, reflection::AttributeValue>
auto Fields(Record& value) {
  return std::tie(value.type, value.value);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, reflection::Attribute>
auto Fields(Record& value) {
  return std::tie(value.target, value.type, value.fields);
}

template <class Record>
auto CompilationEnvironmentFields(Record& value) {
  return std::tie(value.source_paths, value.classes, value.strings, value.array_types,
                  value.global_initializers, value.runtime_types, value.collection_types,
                  value.members, value.layouts, value.declarations, value.interface_declarations,
                  value.global_declarations, value.selector_declarations, value.native_mode,
                  value.global_flags, value.json_decode_plans, value.reflection_types,
                  value.reflection_members, value.attributes);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, ir::Program>
auto Fields(Record& value) {
  return std::tuple_cat(CompilationEnvironmentFields(value),
                        std::tie(value.functions, value.revision, value.source_texts));
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, Fingerprint>
auto Fields(Record& value) {
  return std::tie(value.low, value.high);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, FunctionCodeInputs>
auto Fields(Record& value) {
  return std::tie(value.fingerprint, value.dependencies);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, native::FunctionEntrySymbols>
auto Fields(Record& value) {
  return std::tie(value.external, value.reflection, value.typed, value.implementations);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, FunctionCompilationDescription>
auto Fields(Record& value) {
  return std::tie(value.symbols, value.inputs, value.image);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, frontend::TypeParameterId>
auto Fields(Record& value) {
  return std::tie(value.owner, value.index);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, frontend::TypeName>
auto Fields(Record& value) {
  return std::tie(value.name, value.location, value.kind, value.arguments, value.parameter);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, frontend::ReflectionMemberSelection>
auto Fields(Record& value) {
  return std::tie(value.kind, value.name, value.type_arguments, value.location);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, frontend::ReflectionSelection>
auto Fields(Record& value) {
  return std::tie(value.module, value.type, value.members, value.source_path, value.location);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, frontend::DiagnosticOptions>
auto Fields(Record& value) {
  return std::tie(value.implicit_dynamic_conversion);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, frontend::Diagnostic>
auto Fields(Record& value) {
  return std::tie(value.location, value.message, value.source_path, value.kind, value.severity,
                  value.code, value.length);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, SourceChange>
auto Fields(Record& value) {
  return std::tie(value.path, value.text);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, SourceUpdate>
auto Fields(Record& value) {
  return std::tie(value.base_revision, value.changes, value.added_roots, value.reflection);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, SourceCandidateError>
auto Fields(Record& value) {
  return std::tie(value.status, value.diagnostics, value.declaration);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, frontend::TypeIdentity>
auto Fields(Record& value) {
  return std::tie(value.kind, value.declaration, value.builtin, value.arguments);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, frontend::DeclarationKey>
auto Fields(Record& value) {
  return std::tie(value.owner, value.kind, value.name, value.arguments);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, frontend::DeclarationRecord>
auto Fields(Record& value) {
  return std::tie(value.key, value.definition, value.seen, value.retired);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, SourceSessionConfiguration>
auto Fields(Record& value) {
  return std::tie(value.entry, value.diagnostics, value.reflection, value.imports);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, DevelopmentChanges>
auto Fields(Record& value) {
  return std::tie(value.compiled_functions, value.registered_types, value.withdrawn_types,
                  value.registered_members, value.withdrawn_members, value.changed_members,
                  value.deleted_types, value.deleted_members);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, SourcePreparationRequest>
auto Fields(Record& value) {
  return std::tie(value.committed, value.update, value.retained, value.report_progress);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, PreparedSourceDescription>
auto Fields(Record& value) {
  return std::tie(value.token, value.revision, value.changed_sources, value.sources, value.roots,
                  value.standard_names, value.changes, value.diagnostics, value.reflection,
                  value.identities, value.loading, value.checking, value.ir);
}

template <class Record>
  requires std::is_same_v<std::remove_cv_t<Record>, FileOperationResponse>
auto Fields(Record& value) {
  return std::tie(value.diagnostics, value.update, value.entry);
}

struct WireRecords {
  // 此 IPC schema 中的枚举均从零连续编号；最大值随所属领域定义演进。
  static constexpr auto EnumMaximum(CallableMode /*kind*/) { return CallableMode::VirtualMethod; }
  static constexpr auto EnumMaximum(MemberKind /*unused*/) { return MemberKind::Property; }
  static constexpr auto EnumMaximum(LanguageTypeKind /*unused*/) {
    return LanguageTypeKind::Parameter;
  }
  static constexpr auto EnumMaximum(InlineSlotKind /*unused*/) {
    return InlineSlotKind::NullableDouble;
  }
  static constexpr auto EnumMaximum(layout::Class::Kind /*unused*/) {
    return layout::Class::Kind::Struct;
  }
  static constexpr auto EnumMaximum(ir::Class::Kind /*unused*/) { return ir::Class::Kind::Struct; }
  static constexpr auto EnumMaximum(ir::StructReceiver::Kind /*unused*/) {
    return ir::StructReceiver::Kind::Index;
  }
  static constexpr auto EnumMaximum(Opcode /*unused*/) { return Opcode::Return; }
  static constexpr auto EnumMaximum(JSONDecodeKind /*unused*/) { return JSONDecodeKind::Class; }
  static constexpr auto EnumMaximum(frontend::BindingKind /*unused*/) {
    return frontend::BindingKind::Constant;
  }
  static constexpr auto EnumMaximum(DeclarationKind /*unused*/) {
    return DeclarationKind::EnumMember;
  }
  static constexpr auto EnumMaximum(frontend::Visibility /*unused*/) {
    return frontend::Visibility::Private;
  }
  static constexpr auto EnumMaximum(NativeProgramMode /*unused*/) {
    return NativeProgramMode::Reloadable;
  }
  static constexpr auto EnumMaximum(reflection::TypeKind /*unused*/) {
    return reflection::TypeKind::Never;
  }
  static constexpr auto EnumMaximum(reflection::MemberKind /*unused*/) {
    return reflection::MemberKind::EnumMember;
  }
  static constexpr auto EnumMaximum(frontend::TypeName::Kind /*unused*/) {
    return frontend::TypeName::Kind::Parameter;
  }
  static constexpr auto EnumMaximum(frontend::ReflectionMemberKind /*unused*/) {
    return frontend::ReflectionMemberKind::EnumMember;
  }
  static constexpr auto EnumMaximum(frontend::DiagnosticKind /*unused*/) {
    return frontend::DiagnosticKind::Internal;
  }
  static constexpr auto EnumMaximum(frontend::DiagnosticSeverity /*unused*/) {
    return frontend::DiagnosticSeverity::Error;
  }
  static constexpr auto EnumMaximum(frontend::DiagnosticCode /*unused*/) {
    return frontend::DiagnosticCode::UnusedImport;
  }
  static constexpr auto EnumMaximum(SourceCandidateStatus /*unused*/) {
    return SourceCandidateStatus::RequiresReload;
  }
  static constexpr auto EnumMaximum(frontend::TypeIdentity::Kind /*unused*/) {
    return frontend::TypeIdentity::Kind::Generic;
  }
  template <class T>
  static constexpr bool IsValidEnum(T value) {
    const auto raw = std::to_underlying(value);
    static_assert(std::is_unsigned_v<decltype(raw)>);
    return raw <= std::to_underlying(EnumMaximum(value));
  }
  template <class T>
  static auto Record(T& value) -> decltype(Fields(value)) {
    return Fields(value);
  }
};
struct TransferRecords final : WireRecords {
  template <class Encoder>
  void WriteCustom(Encoder& encoder, const std::shared_ptr<frontend::StructLayout>& value) {
    encoder.Write(value != nullptr);
    if (value == nullptr) {
      return;
    }
    const auto [found, inserted] =
        layouts.try_emplace(value.get(), static_cast<std::uint32_t>(layouts.size()));
    encoder.Write(found->second);
    if (inserted) {
      encoder.Write(*value);
    }
  }

  absl::flat_hash_map<const frontend::StructLayout*, std::uint32_t> layouts;
};

using Encoder = BinaryEncoder<TransferRecords>;
// 指纹只依赖布局内容，不依赖消息中对象是否共享同一地址。
using ContentEncoder = BinaryEncoder<WireRecords>;

class Decoder final : public BinaryDecoder<Decoder, WireRecords> {
 public:
  explicit Decoder(std::span<const std::byte> bytes) : BinaryDecoder(bytes) {}
  void ReadCustom(std::shared_ptr<const ir::StructReceiver>& value) {
    bool present = false;
    Read(present);
    if (!present || !CanAllocate(1, sizeof(ir::StructReceiver))) {
      return;
    }
    auto receiver = std::make_shared<ir::StructReceiver>();
    Read(*receiver);
    value = std::move(receiver);
  }
  void ReadCustom(std::shared_ptr<frontend::StructLayout>& value) {
    bool present = false;
    Read(present);
    if (HasError() || !present) {
      return;
    }
    std::uint32_t layout_id = 0;
    Read(layout_id);
    if (HasError()) {
      return;
    }
    if (layout_id < layouts_.size()) {
      const auto& layout = layouts_[layout_id];
      if (layout == nullptr) {
        Fail(reader().Error("cyclic struct layout reference"));
      } else {
        value = layout;
      }
      return;
    }
    if (layout_id != layouts_.size()) {
      Fail(reader().Error("invalid struct layout reference"));
      return;
    }
    if (!CanAllocate(1, sizeof(frontend::StructLayout) + sizeof(value))) {
      return;
    }
    // 先保留编号，完成后才发布；拒绝环和前向引用，不留下 shared_ptr 所有权环。
    layouts_.emplace_back();
    auto layout = std::make_shared<frontend::StructLayout>();
    Read(*layout);
    if (!HasError()) {
      layouts_[layout_id] = layout;
      value = std::move(layout);
    }
  }
  void ReadCustom(std::string_view& value) {
    std::string text;
    Read(text);
    if (!HasError()) {
      value = frontend::InternTypeName(std::move(text));
    }
  }
  template <class T>
  std::expected<T, std::string> Decode() {
    T value;
    Read(value);
    if (auto finished = Finish(); !finished) {
      return std::unexpected(std::move(finished.error()));
    }
    return value;
  }

 private:
  std::vector<std::shared_ptr<frontend::StructLayout>> layouts_;
};

constexpr auto kMaximumCompilerFrame = kMaximumBinaryPayloadBytes;

}  // namespace

BinaryWriter CompilerRequest(CompilerCommand command, std::uint64_t resource_id) {
  BinaryWriter writer;
  writer.Write(static_cast<std::uint8_t>(command));
  writer.Write(resource_id);
  return writer;
}

std::vector<std::byte> EncodeCompilationEnvironment(const ir::Program& program) {
  // 环境投影与完整 Program 编码共用字段表，排除函数体和调试文本。
  // 两种指纹还绑定 kCompilerBuildIdentity，不跨编译器 schema 复用缓存。
  BinaryWriter writer;
  ContentEncoder encoder(writer);
  std::apply([&](const auto&... fields) { (encoder.Write(fields), ...); },
             CompilationEnvironmentFields(program));
  for (const auto& function : program.functions) {
    encoder.Write(function.name);
    encoder.Write(function.return_type);
    encoder.Write(function.parameter_count);
    encoder.Write(function.native_symbol);
    encoder.Write(function.is_async);
    encoder.Write(function.is_exported);
    encoder.Write(function.is_generated_constructor);
    encoder.Write(function.visibility);
    encoder.Write(function.signature);
    for (const auto& type : std::span(function.value_types).first(function.parameter_count)) {
      encoder.Write(type);
    }
  }
  return std::move(writer).Take();
}
void EncodeFunctions(BinaryWriter& writer,
                     const FunctionCompiler& compiler,
                     const std::vector<std::byte>& image) {
  Encoder encoder(writer);
  encoder.Write(compiler.symbols);
  encoder.Write(compiler.inputs);
  encoder.Write(image);
}
void EncodeSourceResponse(BinaryWriter& writer, const SourcePreparationResponse& response) {
  Encoder(writer).Write(response);
}
void EncodeFileResponse(BinaryWriter& writer, const FileOperationResponse& response) {
  Encoder(writer).Write(response);
}
std::expected<FileOperationResponse, std::string> DecodeFileResponse(
    std::span<const std::byte> bytes) {
  return Decoder(bytes).Decode<FileOperationResponse>();
}
void EncodeSourceUpdate(BinaryWriter& writer, const std::optional<SourceUpdate>& update) {
  Encoder(writer).Write(update);
}
std::expected<std::optional<SourceUpdate>, std::string> DecodeSourceUpdate(
    std::span<const std::byte> bytes) {
  return Decoder(bytes).Decode<std::optional<SourceUpdate>>();
}
std::expected<SourcePreparationResponse, std::string> DecodeSourceResponse(
    std::span<const std::byte> bytes) {
  return Decoder(bytes).Decode<SourcePreparationResponse>();
}
std::expected<FunctionCompilationDescription, std::string> DecodeFunctions(
    std::span<const std::byte> bytes) {
  return Decoder(bytes).Decode<FunctionCompilationDescription>();
}

std::vector<std::byte> EncodeProgram(const ir::Program& program) {
  BinaryWriter writer;
  EncodeProgram(writer, program);
  return std::move(writer).Take();
}
void EncodeProgram(BinaryWriter& writer, const ir::Program& program) {
  Encoder(writer).Write(program);
}
std::expected<ir::Program, std::string> DecodeProgram(std::span<const std::byte> bytes) {
  return Decoder(bytes).Decode<ir::Program>();
}

std::vector<std::byte> EncodeFunction(const ir::Function& function) {
  BinaryWriter writer;
  ContentEncoder(writer).Write(function);
  return std::move(writer).Take();
}

void EncodeSourceConfiguration(BinaryWriter& writer,
                               const SourceSessionConfiguration& configuration) {
  Encoder(writer).Write(configuration);
}
std::expected<SourceSessionConfiguration, std::string> DecodeSourceConfiguration(
    std::span<const std::byte> bytes) {
  return Decoder(bytes).Decode<SourceSessionConfiguration>();
}

void EncodeSourceRequest(BinaryWriter& writer, const SourcePreparationRequest& request) {
  Encoder(writer).Write(request);
}
std::expected<SourcePreparationRequest, std::string> DecodeSourceRequest(
    std::span<const std::byte> bytes) {
  return Decoder(bytes).Decode<SourcePreparationRequest>();
}

std::expected<std::vector<std::byte>, std::string> ReadCompilerFrame(std::FILE* input) {
  std::array<std::byte, sizeof(std::uint64_t)> header{};
  if (std::fread(header.data(), header.size(), 1, input) != 1) {
    return std::unexpected("compiler process closed its pipe");
  }
  BinaryReader reader(header);
  const auto size = *reader.Read<std::uint64_t>();
  if (size > kMaximumCompilerFrame) {
    return std::unexpected("compiler frame exceeds limit");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty() && std::fread(bytes.data(), 1, bytes.size(), input) != bytes.size()) {
    return std::unexpected("incomplete compiler frame");
  }
  return bytes;
}
std::expected<void, std::string> WriteCompilerFrame(std::FILE* output,
                                                    std::span<const std::byte> bytes) {
  const auto size = static_cast<std::uint64_t>(bytes.size());
  if (bytes.size() > kMaximumCompilerFrame) {
    return std::unexpected("compiler frame exceeds limit");
  }
  BinaryWriter header;
  header.Write(size);
#ifndef _WIN32
  // 编译进程退出后返回 I/O 错误；不能让断开的管道终止宿主，也不改变全局信号策略。
  sigset_t pipe{};
  sigset_t previous{};
  sigset_t pending{};
  sigemptyset(&pipe);
  sigaddset(&pipe, SIGPIPE);
  const auto block_error = pthread_sigmask(SIG_BLOCK, &pipe, &previous);
  if (block_error != 0) {
    return std::unexpected("cannot block compiler pipe signal");
  }
  sigpending(&pending);
  const bool was_pending = sigismember(&pending, SIGPIPE) != 0;
#endif
  const bool failed =
      std::fwrite(header.bytes().data(), header.bytes().size(), 1, output) != 1 ||
      (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), output) != bytes.size()) ||
      std::fflush(output) != 0;
#ifndef _WIN32
  sigpending(&pending);
  if (!was_pending && sigismember(&pending, SIGPIPE) != 0) {
    int consumed = 0;
    sigwait(&pipe, &consumed);
  }
  const auto restore_error = pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  if (restore_error != 0) {
    return std::unexpected(
        failed ? "cannot write compiler pipe; cannot restore compiler pipe signal mask"
               : "cannot restore compiler pipe signal mask");
  }
#endif
  if (failed) {
    return std::unexpected("cannot write compiler pipe");
  }
  return {};
}

}  // namespace vela::compiler::jit
