#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Type.h>
#include <llvm/ADT/DenseMap.h>

#include "compiler/bindgen/model.h"

namespace vela::bindgen {

enum class TypePosition : std::uint8_t { Parameter, Result };

using HostTypes = llvm::DenseMap<const clang::CXXRecordDecl*, std::string>;

[[nodiscard]] std::string PrintType(clang::QualType type, const clang::ASTContext& context);
[[nodiscard]] bool IsContext(clang::QualType type);
[[nodiscard]] clang::QualType UnwrapExpected(clang::QualType type);
[[nodiscard]] std::expected<Type, std::string> MapType(clang::QualType type,
                                                       TypePosition position,
                                                       clang::ASTContext& context,
                                                       const HostTypes& hosts);

}  // namespace vela::bindgen
