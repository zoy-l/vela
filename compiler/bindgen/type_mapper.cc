#include "compiler/bindgen/type_mapper.h"

#include <optional>
#include <string>
#include <string_view>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/QualTypeNames.h>
#include <clang/AST/TemplateBase.h>
#include <clang/AST/Type.h>
#include <clang/Basic/TargetInfo.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>

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
  return Type{
      .kind = TypeKind::Host,
      .ownership = ownership,
      .key = found->second,
      .cpp = {},
      .nullable = false,
      .is_const = type.isConstQualified(),
  };
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
  if (result) {
    result->nullable = true;
  }
  return result;
}

std::optional<Type> MapCanonical(clang::QualType type,
                                 TypePosition position,
                                 clang::ASTContext& context,
                                 const HostTypes& hosts) {
  if (type->isBooleanType()) {
    return Type{.kind = TypeKind::Boolean, .key = {}, .cpp = {}};
  }
  const auto integer = IntegerType(context);
  if (!integer.isNull() && context.hasSameType(type, integer)) {
    return Type{.kind = TypeKind::Integer, .key = {}, .cpp = {}};
  }
  if (context.hasSameType(type, context.DoubleTy)) {
    return Type{.kind = TypeKind::Double, .key = {}, .cpp = {}};
  }
  const auto* record = type->getAsCXXRecordDecl();
  if (record != nullptr &&
      record->getQualifiedNameAsString() == "vela::runtime::CancellationToken") {
    return Type{.kind = TypeKind::Cancellation, .key = {}, .cpp = {}};
  }
  if (position == TypePosition::Parameter) {
    if (IsString(type, "basic_string_view")) {
      return Type{.kind = TypeKind::String, .key = {}, .cpp = {}};
    }
    if (type->isPointerType() || type->isLValueReferenceType()) {
      auto result = HostType(type->getPointeeType(), Ownership::Borrow, hosts);
      if (result) {
        result->nullable = type->isPointerType();
      }
      return result;
    }
    return std::nullopt;
  }
  if (type->isVoidType()) {
    return Type{};
  }
  if (IsString(type, "basic_string")) {
    return Type{.kind = TypeKind::String, .key = {}, .cpp = {}};
  }
  const auto* optional = StandardTemplate(type, "optional");
  if (optional != nullptr) {
    const auto payload = Argument(*optional, 0);
    if (!payload.isNull() && IsString(payload, "basic_string")) {
      return Type{.kind = TypeKind::String, .key = {}, .cpp = {}, .nullable = true};
    }
    return std::nullopt;
  }
  return OwnedType(type, hosts);
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
  if (record == nullptr || record->getQualifiedNameAsString() != "vela::runtime::NativeError") {
    return {};
  }
  return Argument(*specialization, 0);
}

std::optional<Type> MapType(clang::QualType type,
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
