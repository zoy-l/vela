#include "compiler/bindgen/type_mapper.h"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/QualTypeNames.h>
#include <clang/AST/TemplateBase.h>
#include <clang/AST/Type.h>
#include <clang/Basic/TargetInfo.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>

#include "base/layout_description.h"
#include "compiler/bindgen/model.h"

namespace vela::bindgen {
namespace {

clang::QualType IntegerType(const clang::ASTContext& context) {
  switch (context.getTargetInfo().getInt64Type()) {
    case clang::TargetInfo::SignedLong:
      return context.LongTy;
    case clang::TargetInfo::SignedLongLong:
      return context.LongLongTy;
    default:
      return {};
  }
}

std::optional<layout::Type::Kind> ScalarKind(clang::QualType type,
                                             const clang::ASTContext& context) {
  using Kind = layout::Type::Kind;
  if (type->isBooleanType()) {
    return Kind::Boolean;
  }
  const auto integer = IntegerType(context);
  if (!integer.isNull() && context.hasSameType(type, integer)) {
    return Kind::Integer;
  }
  return context.hasSameType(type, context.DoubleTy) ? std::optional(Kind::Double) : std::nullopt;
}

const clang::ClassTemplateSpecializationDecl* StandardTemplate(clang::QualType type,
                                                               std::string_view name) {
  const auto* record = type->getAsCXXRecordDecl();
  const auto* specialization =
      llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(record);
  return specialization != nullptr && specialization->isInStdNamespace() &&
                 specialization->getName() == llvm::StringRef(name)
             ? specialization
             : nullptr;
}

clang::QualType Argument(const clang::ClassTemplateSpecializationDecl& type, unsigned index) {
  const auto& arguments = type.getTemplateArgs();
  if (index >= arguments.size() || arguments[index].getKind() != clang::TemplateArgument::Type) {
    return {};
  }
  return arguments[index].getAsType().getCanonicalType();
}

bool IsCharTemplate(clang::QualType type, std::string_view name) {
  const auto* specialization = StandardTemplate(type, name);
  if (specialization == nullptr) {
    return false;
  }
  const auto character = Argument(*specialization, 0);
  return !character.isNull() && (character->isSpecificBuiltinType(clang::BuiltinType::Char_S) ||
                                 character->isSpecificBuiltinType(clang::BuiltinType::Char_U));
}

bool IsString(clang::QualType type, std::string_view name) {
  const auto* specialization = StandardTemplate(type, name);
  if (specialization == nullptr || !IsCharTemplate(type, name)) {
    return false;
  }
  const auto traits = Argument(*specialization, 1);
  if (traits.isNull() || !IsCharTemplate(traits, "char_traits")) {
    return false;
  }
  if (name == "basic_string_view") {
    return true;
  }
  const auto allocator = Argument(*specialization, 2);
  return !allocator.isNull() && IsCharTemplate(allocator, "allocator");
}

std::optional<Type> HostType(clang::QualType type, Ownership ownership, const HostTypes& hosts) {
  const auto* record = type->getAsCXXRecordDecl();
  if (record == nullptr || type.isVolatileQualified()) {
    return std::nullopt;
  }
  const auto found = hosts.find(record->getCanonicalDecl());
  if (found == hosts.end()) {
    return std::nullopt;
  }
  auto result = bindgen::HostType(found->second, ownership);
  result.is_const = type.isConstQualified();
  return result;
}

std::optional<Type> OwnedType(clang::QualType type, const HostTypes& hosts) {
  const auto* unique = StandardTemplate(type, "unique_ptr");
  const auto* shared = StandardTemplate(type, "shared_ptr");
  if (unique == nullptr && shared == nullptr) {
    return std::nullopt;
  }
  const auto object = Argument(unique != nullptr ? *unique : *shared, 0);
  if (object.isNull() || object.isConstQualified()) {
    return std::nullopt;
  }
  if (unique != nullptr) {
    const auto deleter = Argument(*unique, 1);
    const auto* standard = deleter.isNull() ? nullptr : StandardTemplate(deleter, "default_delete");
    if (standard == nullptr || Argument(*standard, 0) != object) {
      return std::nullopt;
    }
  }
  auto result = HostType(object, unique != nullptr ? Ownership::Unique : Ownership::Shared, hosts);
  if (result && result->value) {
    result->value->nullable = true;
  }
  return result;
}

Type Leaf(layout::Type::Kind kind) {
  return {.layouts = {}, .value = layout::Type{.kind = kind}, .cpp = {}};
}

const clang::ClassTemplateSpecializationDecl* VelaTemplate(clang::QualType type,
                                                           std::string_view name) {
  const auto* record = type->getAsCXXRecordDecl();
  const auto* specialization =
      llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(record);
  if (specialization == nullptr) {
    return nullptr;
  }
  const auto* declaration = specialization->getSpecializedTemplate();
  const auto* owner = llvm::dyn_cast<clang::NamespaceDecl>(declaration->getDeclContext());
  constexpr std::string_view kNamespacePrefix = "vela::";
  return declaration->getName() == llvm::StringRef(name).drop_front(kNamespacePrefix.size()) &&
                 owner != nullptr && owner->getName() == "vela" &&
                 owner->getParent()->isTranslationUnit()
             ? specialization
             : nullptr;
}

using MappedType = std::expected<Type, std::string>;

std::unexpected<std::string> Unsupported(clang::QualType type,
                                         const clang::ASTContext& context,
                                         std::string_view reason = "unsupported logical type") {
  return std::unexpected((type.isNull() ? std::string("<missing>") : PrintType(type, context)) +
                         " (" + std::string(reason) + ")");
}

MappedType At(MappedType mapped, std::string_view path) {
  if (!mapped) {
    return std::unexpected(std::string(path) + ": " + std::move(mapped.error()));
  }
  return mapped;
}

MappedType Require(std::optional<Type> mapped,
                   clang::QualType type,
                   const clang::ASTContext& context) {
  return mapped ? MappedType(std::move(*mapped)) : MappedType(Unsupported(type, context));
}

const clang::CXXRecordDecl* ScriptTraits(const clang::ClassTemplateSpecializationDecl& wrapper,
                                         clang::ASTContext& context) {
  const auto tag = Argument(wrapper, 0);
  auto* scope = wrapper.getSpecializedTemplate()->getDeclContext();
  const auto name = context.DeclarationNames.getIdentifier(&context.Idents.get("ScriptTypeTraits"));
  for (auto* declaration : scope->lookup(name)) {
    auto* primary = llvm::dyn_cast<clang::ClassTemplateDecl>(declaration);
    if (primary == nullptr) {
      continue;
    }
    void* position = nullptr;
    const std::array arguments{clang::TemplateArgument(tag)};
    const auto* specialization = primary->findSpecialization(arguments, position);
    return specialization == nullptr ? nullptr : specialization->getDefinition();
  }
  return nullptr;
}

std::optional<std::string> ConstantText(const clang::Expr* expression) {
  if (expression == nullptr) {
    return std::nullopt;
  }
  expression = expression->IgnoreParenImpCasts()->IgnoreImplicit();
  if (const auto* text = llvm::dyn_cast<clang::StringLiteral>(expression)) {
    return text->getString().str();
  }
  if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
    const auto* variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
    return variable == nullptr ? std::nullopt : ConstantText(variable->getAnyInitializer());
  }
  if (const auto* constructed = llvm::dyn_cast<clang::CXXConstructExpr>(expression);
      constructed != nullptr && IsString(constructed->getType(), "basic_string_view") &&
      constructed->getNumArgs() != 0) {
    return ConstantText(constructed->getArg(0));
  }
  return std::nullopt;
}

MappedType LogicalType(clang::QualType type, clang::ASTContext& context, const HostTypes& hosts);

struct ScriptTraitsInfo final {
  const clang::VarDecl* kind = nullptr;
  const clang::VarDecl* name = nullptr;
  const clang::VarDecl* native_key = nullptr;
  const clang::VarDecl* module = nullptr;
  const clang::TypeAliasDecl* arguments = nullptr;
};

ScriptTraitsInfo ReadScriptTraits(const clang::CXXRecordDecl& traits) {
  ScriptTraitsInfo result;
  for (const auto* declaration : traits.decls()) {
    if (const auto* alias = llvm::dyn_cast<clang::TypeAliasDecl>(declaration);
        alias != nullptr && alias->getName() == "Arguments") {
      result.arguments = alias;
    }
    const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration);
    if (variable == nullptr) {
      continue;
    }
    if (variable->getName() == "kKind") {
      result.kind = variable;
    } else if (variable->getName() == "kName") {
      result.name = variable;
    } else if (variable->getName() == "kNativeKey") {
      result.native_key = variable;
    } else if (variable->getName() == "kModule") {
      result.module = variable;
    }
  }
  return result;
}

std::expected<std::string, std::string> ScriptKind(const clang::VarDecl* kind,
                                                   clang::ASTContext& context) {
  const auto* enumeration = kind == nullptr ? nullptr : kind->getType()->getAs<clang::EnumType>();
  clang::Expr::EvalResult evaluated;
  if (enumeration == nullptr || !kind->isConstexpr() || kind->getAnyInitializer() == nullptr ||
      enumeration->getDecl()->getQualifiedNameAsString() != "vela::ScriptTypeKind" ||
      !kind->getAnyInitializer()->EvaluateAsInt(evaluated, context)) {
    return std::unexpected("ScriptTypeTraits.kKind must be a constant ScriptTypeKind");
  }
  for (const auto* value : enumeration->getDecl()->enumerators()) {
    if (value->getInitVal() == evaluated.Val.getInt()) {
      return value->getNameAsString();
    }
  }
  return std::unexpected("ScriptTypeTraits.kKind is not a declared ScriptTypeKind");
}

std::expected<void, std::string> ImportNominalArguments(const clang::TypeAliasDecl* arguments,
                                                        clang::ASTContext& context,
                                                        const HostTypes& hosts,
                                                        layout::Program& layouts,
                                                        layout::Nominal& nominal) {
  const auto* tuple =
      arguments == nullptr
          ? nullptr
          : StandardTemplate(arguments->getUnderlyingType().getCanonicalType(), "tuple");
  if (tuple == nullptr) {
    return std::unexpected("ScriptTypeTraits.Arguments requires std::tuple types");
  }
  for (const auto& argument : tuple->getTemplateArgs()[0].pack_elements()) {
    auto mapped = LogicalType(argument.getAsType().getCanonicalType(), context, hosts);
    if (!mapped) {
      return std::unexpected("Arguments: " + std::move(mapped.error()));
    }
    const auto& value = mapped->value;
    if (!value) {
      return std::unexpected("Arguments: nominal argument cannot be void");
    }
    nominal.arguments.push_back(layout::ImportType(layouts, mapped->layouts, *value));
  }
  return {};
}

MappedType ScriptNominal(const clang::ClassTemplateSpecializationDecl& wrapper,
                         clang::ASTContext& context,
                         const HostTypes& hosts) {
  const auto tag = Argument(wrapper, 0);
  const auto* traits = ScriptTraits(wrapper, context);
  if (traits == nullptr) {
    return Unsupported(tag, context, "missing ScriptTypeTraits specialization");
  }
  const auto [kind, name, native_key, module, arguments] = ReadScriptTraits(*traits);
  const auto type_kind = ScriptKind(kind, context);
  if (!type_kind) {
    return Unsupported(tag, context, type_kind.error());
  }
  const auto qualified = name == nullptr ? std::nullopt : ConstantText(name->getAnyInitializer());
  if (!qualified || qualified->empty() || !name->isConstexpr()) {
    return Unsupported(tag, context, "ScriptTypeTraits.kName requires a constant declaration name");
  }
  const bool is_enum = *type_kind == "Enum";
  const bool enum_wrapper = wrapper.getSpecializedTemplate()->getName() == "Enum";
  const bool host_wrapper = wrapper.getSpecializedTemplate()->getName() == "Host";
  if (is_enum != enum_wrapper || (*type_kind == "Host") != host_wrapper ||
      (!is_enum && *type_kind != "Class" && *type_kind != "Interface" && *type_kind != "Host")) {
    return Unsupported(tag, context, "Object/Enum/Host must match ScriptTypeTraits.kKind");
  }
  if (host_wrapper) {
    const auto key =
        native_key == nullptr ? std::nullopt : ConstantText(native_key->getAnyInitializer());
    if (!key || key->empty() || !native_key->isConstexpr()) {
      return Unsupported(tag, context, "ScriptTypeTraits.kNativeKey requires a constant host key");
    }
    auto result = bindgen::HostType(*key, Ownership::Local);
    result.layouts.classes.front().name = *qualified;
    return result;
  }
  Type result{.layouts = {}, .cpp = {}};
  const auto source = module == nullptr ? std::nullopt : ConstantText(module->getAnyInitializer());
  if (!source || source->empty() || !module->isConstexpr()) {
    return Unsupported(tag, context, "ScriptTypeTraits.kModule requires a constant module URI");
  }
  layout::Nominal nominal{.module = *source, .name = *qualified, .arguments = {}, .exported = true};
  const auto imported = ImportNominalArguments(arguments, context, hosts, result.layouts, nominal);
  if (!imported) {
    return Unsupported(tag, context, imported.error());
  }
  const auto identity = layout::NominalKey(result.layouts, nominal);
  result.layouts.nominals.emplace(identity, std::move(nominal));
  if (*type_kind == "Interface") {
    result.value = layout::Type{
        .kind = layout::Type::Kind::Interface,
        .index = static_cast<std::uint32_t>(result.layouts.interfaces.size()),
    };
    result.layouts.interfaces.push_back(identity);
  } else {
    result.value = layout::Type{
        .kind = is_enum ? layout::Type::Kind::Enum : layout::Type::Kind::Instance,
        .index = static_cast<std::uint32_t>(result.layouts.classes.size()),
    };
    result.layouts.classes.push_back({
        .name = identity,
        .native_key = {},
        .native_contract = {},
        .kind = is_enum ? layout::Class::Kind::Enum : layout::Class::Kind::Instance,
        .reference_fields = {},
        .callable = {},
        .interfaces = {},
        .dispatch_targets = {},
    });
  }
  return result;
}

MappedType LogicalSignature(const clang::FunctionProtoType& function,
                            clang::ASTContext& context,
                            const HostTypes& hosts) {
  if (function.isVariadic()) {
    return std::unexpected("Function: variadic signatures are unsupported");
  }
  Type result{.layouts = {}, .cpp = {}};
  layout::Signature signature{.name = {}, .parameters = {}};
  unsigned index = 0;
  for (const auto parameter : function.param_types()) {
    auto mapped = LogicalType(parameter.getCanonicalType(), context, hosts);
    const auto path = "Function.parameter[" + std::to_string(index++) + "]";
    if (!mapped) {
      return At(std::move(mapped), path);
    }
    const auto& value = mapped->value;
    if (!value) {
      return At(Unsupported(parameter, context, "parameters cannot be void"), path);
    }
    signature.parameters.push_back(
        {.type = layout::ImportType(result.layouts, mapped->layouts, *value)});
  }
  auto mapped_result = LogicalType(function.getReturnType().getCanonicalType(), context, hosts);
  if (!mapped_result) {
    return At(std::move(mapped_result), "Function.result");
  }
  if (mapped_result->value) {
    signature.result =
        layout::ImportType(result.layouts, mapped_result->layouts, *mapped_result->value);
  }
  result.value = layout::Type{
      .kind = layout::Type::Kind::Callable,
      .index = static_cast<std::uint32_t>(result.layouts.signatures.size()),
  };
  result.layouts.signatures.push_back(std::move(signature));
  return result;
}

MappedType MapLogicalContainer(clang::QualType type,
                               const clang::ClassTemplateSpecializationDecl& container,
                               layout::Type::Kind kind,
                               clang::ASTContext& context,
                               const HostTypes& hosts) {
  using Kind = layout::Type::Kind;
  const auto qualified = container.getSpecializedTemplate()->getQualifiedNameAsString();
  auto element = LogicalType(Argument(container, 0), context, hosts);
  const bool async = kind == Kind::Future || kind == Kind::Completer;
  std::string path = container.getSpecializedTemplate()->getNameAsString();
  if (async) {
    path += ".payload";
  } else if (qualified == "vela::Map") {
    path += ".key";
  } else {
    path += ".element";
  }
  if (!element) {
    return At(std::move(element), path);
  }
  const auto& element_type = element->value;
  if (async && element_type && element_type->kind == Kind::Future) {
    return At(Unsupported(type, context, "Future payload types must be flattened"), path);
  }
  Type result{.layouts = {}, .cpp = {}};
  std::optional<layout::Type> imported;
  if (element_type) {
    imported = layout::ImportType(result.layouts, element->layouts, *element_type);
  }
  if (async) {
    result.value = layout::Type{
        .kind = kind,
        .index = static_cast<std::uint32_t>(result.layouts.signatures.size()),
    };
    result.layouts.signatures.push_back({.name = {}, .parameters = {}, .result = imported});
    return result;
  }
  if (!imported) {
    return At(Unsupported(type, context, "container elements cannot be void"), path);
  }
  if (kind == Kind::Array) {
    result.value = layout::Type{
        .kind = kind,
        .index = static_cast<std::uint32_t>(result.layouts.arrays.size()),
    };
    result.layouts.arrays.push_back({.name = {}, .element = *imported});
  } else if (kind == Kind::Collection) {
    layout::Collection collection{.name = {}, .key = *imported};
    if (qualified == "vela::Map") {
      auto value = LogicalType(Argument(container, 1), context, hosts);
      if (!value) {
        return At(std::move(value), "Map.value");
      }
      if (!value->value) {
        return At(Unsupported(type, context, "map values cannot be void"), "Map.value");
      }
      collection.value = layout::ImportType(result.layouts, value->layouts, *value->value);
    }
    result.value = layout::Type{
        .kind = kind,
        .index = static_cast<std::uint32_t>(result.layouts.collections.size()),
    };
    result.layouts.collections.push_back(std::move(collection));
  }
  return result;
}

MappedType LogicalContainer(clang::QualType type,
                            clang::ASTContext& context,
                            const HostTypes& hosts) {
  using Kind = layout::Type::Kind;
  constexpr auto kContainers = std::to_array<std::pair<std::string_view, Kind>>({
      {"vela::Array", Kind::Array},
      {"vela::Map", Kind::Collection},
      {"vela::Set", Kind::Collection},
      {"vela::Future", Kind::Future},
      {"vela::Completer", Kind::Completer},
  });
  for (const auto& [qualified, kind] : kContainers) {
    const auto* container = VelaTemplate(type, qualified);
    if (container == nullptr) {
      continue;
    }
    return MapLogicalContainer(type, *container, kind, context, hosts);
  }
  return Unsupported(type, context);
}

MappedType LogicalNullable(clang::QualType type,
                           const clang::ClassTemplateSpecializationDecl& nullable,
                           clang::ASTContext& context,
                           const HostTypes& hosts) {
  using Kind = layout::Type::Kind;
  auto result = LogicalType(Argument(nullable, 0), context, hosts);
  if (!result) {
    return At(std::move(result), "Nullable.value");
  }
  if (!result->value) {
    return At(Unsupported(type, context, "void cannot be nullable"), "Nullable.value");
  }
  if (result->value->kind != Kind::Dynamic) {
    result->value->nullable = true;
  }
  return result;
}

MappedType LogicalType(clang::QualType type, clang::ASTContext& context, const HostTypes& hosts) {
  using Kind = layout::Type::Kind;
  if (type.isNull() || type.hasQualifiers()) {
    return Unsupported(type, context);
  }
  if (const auto kind = ScalarKind(type, context)) {
    return Leaf(*kind);
  }
  if (type->isVoidType()) {
    return Type{.layouts = {}, .cpp = {}};
  }
  const auto* record = type->getAsCXXRecordDecl();
  if (record == nullptr) {
    return Unsupported(type, context);
  }
  constexpr auto kLeaves = std::to_array<std::pair<std::string_view, Kind>>({
      {"vela::String", Kind::String},
      {"vela::MutableString", Kind::MutableString},
      {"vela::ByteArray", Kind::ByteArray},
      {"vela::ByteData", Kind::ByteData},
      {"vela::RegExp", Kind::RegExp},
      {"vela::RegExpMatch", Kind::RegExpMatch},
      {"vela::Timer", Kind::Timer},
      {"vela::CancellationToken", Kind::CancellationToken},
      {"vela::Value", Kind::Dynamic},
  });
  const auto name = record->getQualifiedNameAsString();
  for (const auto& [qualified, kind] : kLeaves) {
    if (name == qualified) {
      return Leaf(kind);
    }
  }
  if (const auto* host = VelaTemplate(type, "vela::Host")) {
    const auto native = HostType(Argument(*host, 0), Ownership::Local, hosts);
    return At(native ? MappedType(*native) : ScriptNominal(*host, context, hosts), "Host.value");
  }
  if (const auto* object = VelaTemplate(type, "vela::Object")) {
    return At(ScriptNominal(*object, context, hosts), "Object.tag");
  }
  if (const auto* enumeration = VelaTemplate(type, "vela::Enum")) {
    return At(ScriptNominal(*enumeration, context, hosts), "Enum.tag");
  }
  if (const auto* nullable = VelaTemplate(type, "vela::Nullable")) {
    return LogicalNullable(type, *nullable, context, hosts);
  }
  if (const auto* function = VelaTemplate(type, "vela::Function")) {
    const auto signature = Argument(*function, 0);
    const auto* prototype =
        signature.isNull() ? nullptr : signature->getAs<clang::FunctionProtoType>();
    return prototype == nullptr
               ? MappedType(Unsupported(signature, context, "expected a function signature"))
               : LogicalSignature(*prototype, context, hosts);
  }
  return LogicalContainer(type, context, hosts);
}

MappedType MapHostBoundary(clang::QualType type,
                           TypePosition position,
                           clang::ASTContext& context,
                           const HostTypes& hosts) {
  using Kind = layout::Type::Kind;
  if (position == TypePosition::Parameter) {
    if (IsString(type, "basic_string_view")) {
      return Leaf(Kind::String);
    }
    if (type->isPointerType() || type->isLValueReferenceType()) {
      auto result = HostType(type->getPointeeType(), Ownership::Borrow, hosts);
      if (result && result->value) {
        result->value->nullable = type->isPointerType();
      }
      return Require(std::move(result), type, context);
    }
    return Unsupported(type, context);
  }
  if (type->isVoidType()) {
    return Type{};
  }
  if (IsString(type, "basic_string")) {
    return Leaf(Kind::String);
  }
  const auto* optional = StandardTemplate(type, "optional");
  if (optional != nullptr) {
    const auto payload = Argument(*optional, 0);
    if (!payload.isNull() && IsString(payload, "basic_string")) {
      auto result = Leaf(Kind::String);
      result.value = layout::Type{.kind = Kind::String, .nullable = true};
      return result;
    }
    return Unsupported(type, context);
  }
  return Require(OwnedType(type, hosts), type, context);
}

MappedType MapCanonical(clang::QualType type,
                        TypePosition position,
                        clang::ASTContext& context,
                        const HostTypes& hosts) {
  using Kind = layout::Type::Kind;
  if (const auto* local = VelaTemplate(type, "vela::Local")) {
    auto result = LogicalType(Argument(*local, 0), context, hosts);
    if (!result) {
      return At(std::move(result), "Local.value");
    }
    if (!result->value || result->value->nullable || result->value->kind == Kind::Boolean ||
        result->value->kind == Kind::Integer || result->value->kind == Kind::Double) {
      return Unsupported(type, context, "Local requires a nonnullable managed logical type");
    }
    result->ownership = Ownership::Local;
    return result;
  }
  if (const auto* optional = StandardTemplate(type, "optional")) {
    const auto payload = Argument(*optional, 0);
    if (!payload.isNull() &&
        (VelaTemplate(payload, "vela::Local") != nullptr || ScalarKind(payload, context))) {
      auto result = MapCanonical(payload, position, context, hosts);
      if (result && result->value) {
        result->value->nullable = result->value->kind != Kind::Dynamic;
      }
      return At(std::move(result), "optional.value");
    }
  }
  if (const auto kind = ScalarKind(type, context)) {
    return Leaf(*kind);
  }
  const auto* record = type->getAsCXXRecordDecl();
  if (record != nullptr &&
      record->getQualifiedNameAsString() == "vela::runtime::CancellationToken") {
    return Leaf(Kind::CancellationToken);
  }
  return MapHostBoundary(type, position, context, hosts);
}

}  // namespace

std::string PrintType(clang::QualType type, const clang::ASTContext& context) {
  clang::PrintingPolicy policy(context.getLangOpts());
  policy.SuppressTagKeyword = true;
  policy.FullyQualifiedName = true;
  return clang::TypeName::getFullyQualifiedName(type.getCanonicalType(), context, policy, true);
}

bool IsContext(clang::QualType type) {
  if (!type->isLValueReferenceType()) {
    return false;
  }
  const auto pointee = type->getPointeeType();
  const auto* record = pointee->getAsCXXRecordDecl();
  return !pointee.hasQualifiers() && record != nullptr &&
         record->getQualifiedNameAsString() == "vela::runtime::NativeContext";
}

clang::QualType UnwrapExpected(clang::QualType type) {
  const auto* specialization = StandardTemplate(type, "expected");
  if (specialization == nullptr) {
    return {};
  }
  const auto error = Argument(*specialization, 1);
  const auto* record = error.isNull() ? nullptr : error->getAsCXXRecordDecl();
  if (record == nullptr || record->getQualifiedNameAsString() != "vela::Error") {
    return {};
  }
  return Argument(*specialization, 0);
}

std::expected<Type, std::string> MapType(clang::QualType type,
                                         TypePosition position,
                                         clang::ASTContext& context,
                                         const HostTypes& hosts) {
  const auto canonical = type.getCanonicalType().getLocalUnqualifiedType();
  auto result = MapCanonical(canonical, position, context, hosts);
  if (result) {
    result->cpp = PrintType(type, context);
  }
  return result;
}

}  // namespace vela::bindgen
