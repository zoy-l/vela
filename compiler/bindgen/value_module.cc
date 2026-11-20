#include "compiler/bindgen/value_module.h"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/TemplateBase.h>
#include <clang/AST/Type.h>
#include <clang/Basic/Specifiers.h>
#include <clang/Frontend/CompilerInstance.h>
#include <llvm/Support/Casting.h>

#include "base/fingerprint.h"
#include "base/layout_description.h"
#include "base/native_binding.h"
#include "base/path.h"
#include "compiler/bindgen/collector.h"
#include "compiler/bindgen/model.h"
#include "compiler/bindgen/type_mapper.h"
#include "compiler/frontend/syntax/names.h"

namespace vela::bindgen {
namespace {

class ValueCollector final {
 public:
  ValueCollector(std::string key, clang::CompilerInstance& compiler, Collection& collection)
      : key_(std::move(key)),
        compiler_(&compiler),
        context_(&compiler.getASTContext()),
        collection_(&collection) {}

  void Run(clang::CXXRecordDecl& declaration) {
    Class module{
        .key = key_,
        .name = key_.substr(key_.rfind('.') + 1),
        .cpp = declaration.getQualifiedNameAsString(),
        .documentation = {},
        .source = {},
        .members = {},
        .value_cpp = std::nullopt,
        .enumerators = {},
    };
    for (const auto* member : declaration.decls()) {
      if (member->isImplicit() || member->getAccess() != clang::AS_public) {
        continue;
      }
      const auto* variable = llvm::dyn_cast<clang::VarDecl>(member);
      if (variable == nullptr || !variable->isConstexpr() || !variable->hasInit()) {
        Error(*member, "value module exports must be constexpr native function pointers");
        continue;
      }
      const auto* expression = variable->getInit()->IgnoreParenImpCasts();
      if (const auto* address = llvm::dyn_cast<clang::UnaryOperator>(expression);
          address != nullptr && address->getOpcode() == clang::UO_AddrOf) {
        expression = address->getSubExpr()->IgnoreParenImpCasts();
      }
      const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression);
      const auto* function = reference == nullptr
                                 ? nullptr
                                 : llvm::dyn_cast<clang::FunctionDecl>(reference->getDecl());
      if (function == nullptr) {
        Error(*member, "module export must directly reference a native function or specialization");
        continue;
      }
      CollectFunction(*variable, *function, module);
    }
    collection_->module.classes.push_back(std::move(module));
  }

 private:
  void Error(const clang::Decl& declaration, const std::string& message) {
    auto& diagnostics = compiler_->getDiagnostics();
    diagnostics.Report(declaration.getLocation(),
                       diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error, "%0"))
        << message;
  }

  [[nodiscard]] std::string Cpp(clang::QualType type) const {
    return PrintType(type.getNonReferenceType().getUnqualifiedType(), *context_);
  }

  static std::string Codec(const std::string& cpp) {
    return "::vela::bindings::ValueCodec<" + cpp + ">";
  }

  Type Map(clang::QualType input) {
    const auto type = input.getNonReferenceType().getUnqualifiedType().getCanonicalType();
    const auto cpp = Cpp(type);
    Type result{.layouts = {}, .cpp = Codec(cpp) + "::Argument"};
    using Kind = layout::Type::Kind;
    if (type->isVoidType()) {
      return result;
    }
    if (type->isBooleanType() || type->isIntegerType() || type->isFloatingType()) {
      auto kind = Kind::Double;
      if (type->isBooleanType()) {
        kind = Kind::Boolean;
      } else if (type->isIntegerType()) {
        kind = Kind::Integer;
      }
      result.value = layout::Type{.kind = kind};
      return result;
    }
    if (const auto* enumeration = type->getAs<clang::EnumType>()) {
      return MapEnum(*enumeration->getDecl(), cpp);
    }
    const auto* record = type->getAsCXXRecordDecl();
    if (record == nullptr) {
      Error(*root_, "unsupported value type: " + cpp);
      return result;
    }
    if (const auto* specialization =
            llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record)) {
      if (auto mapped = MapSpecialization(*specialization, cpp)) {
        return std::move(*mapped);
      }
    }
    return MapRecord(*record, cpp);
  }

  std::optional<Type> MapSpecialization(const clang::ClassTemplateSpecializationDecl& record,
                                        const std::string& cpp) {
    Type result{.layouts = {}, .cpp = Codec(cpp) + "::Argument"};
    const auto& arguments = record.getTemplateArgs();
    const auto name = record.getName();
    if (record.isInStdNamespace()) {
      if (name == "basic_string" || name == "basic_string_view") {
        result.value = layout::Type{.kind = layout::Type::Kind::String};
        return result;
      }
      if (name == "optional") {
        result = Map(arguments[0].getAsType());
        result.cpp = Codec(cpp) + "::Argument";
        if (result.value) {
          result.value->nullable = true;
        }
        return result;
      }
      if (name == "vector") {
        auto element = Map(arguments[0].getAsType());
        if (!element.value) {
          Error(*root_, "native array elements must have a value type");
          return result;
        }
        result.value = layout::Type{.kind = layout::Type::Kind::Array};
        result.layouts.arrays.push_back({
            .name = {},
            .element = layout::ImportType(result.layouts, element.layouts, *element.value),
        });
        return result;
      }
    }
    const auto qualified = record.getSpecializedTemplate()->getQualifiedNameAsString();
    if (collection_->callbacks.contains(qualified)) {
      return MapCallback(arguments, cpp);
    }
    return std::nullopt;
  }

  Type MapCallback(const clang::TemplateArgumentList& arguments, const std::string& cpp) {
    Type result{.layouts = {}, .cpp = Codec(cpp) + "::Argument"};
    layout::Signature signature{.name = {}, .parameters = {}};
    const auto add = [&](const clang::TemplateArgument& parameter) {
      auto mapped = Map(parameter.getAsType());
      if (!mapped.value) {
        Error(*root_, "native callback parameters must have a value type");
        return;
      }
      signature.parameters.push_back({
          .type = layout::ImportType(result.layouts, mapped.layouts, *mapped.value),
      });
    };
    for (const auto& argument : arguments.asArray()) {
      if (argument.getKind() == clang::TemplateArgument::Pack) {
        for (const auto& parameter : argument.pack_elements()) {
          add(parameter);
        }
      } else {
        add(argument);
      }
    }
    result.value = layout::Type{.kind = layout::Type::Kind::Callable};
    result.layouts.signatures.push_back(std::move(signature));
    return result;
  }

  Type MapRecord(const clang::CXXRecordDecl& record, const std::string& cpp) {
    const auto found = values_.find(cpp);
    if (found != values_.end()) {
      auto host = HostType(found->second, Ownership::Local);
      host.cpp = Codec(cpp) + "::Argument";
      return host;
    }
    auto name = record.getNameAsString();
    const bool standard = record.isInStdNamespace();
    if (standard || llvm::isa<clang::ClassTemplateSpecializationDecl>(record)) {
      name = "NativeValue" + ContentFingerprint(cpp).Hex();
    }
    const auto scope = key_.substr(0, key_.rfind('.'));
    const auto key = scope + '.' + name;
    values_.emplace(cpp, key);
    Class output{
        .key = key,
        .name = name,
        .cpp = "vela::bindings::ValueBox<" + cpp + ">",
        .documentation = {},
        .source = {},
        .members = {},
        .value_cpp = cpp,
        .enumerators = {},
    };
    if (!standard && !collection_->opaque.contains(record.getQualifiedNameAsString()) &&
        record.hasDefinition()) {
      AddRecordMembers(output, *record.getDefinition());
    }
    collection_->module.classes.push_back(std::move(output));
    auto host = HostType(key, Ownership::Local);
    host.cpp = Codec(cpp) + "::Argument";
    return host;
  }

  void AddRecordMembers(Class& output, const clang::CXXRecordDecl& definition) {
    bool constructible = !definition.hasUserDeclaredConstructor();
    for (const auto* constructor : definition.ctors()) {
      if (constructor->isDefaultConstructor() && !constructor->isDeleted() &&
          constructor->getAccess() == clang::AS_public) {
        constructible = true;
      }
    }
    if (constructible && !definition.isAbstract()) {
      AddConstructor(output);
    }
    for (const auto* field : definition.fields()) {
      if (field->getAccess() == clang::AS_public) {
        AddField(output, *field);
      }
    }
  }

  Type MapEnum(const clang::EnumDecl& enumeration, const std::string& cpp) {
    const auto name = enumeration.getNameAsString();
    if (enums_.insert(cpp).second) {
      Class output{
          .key = key_.substr(0, key_.rfind('.')) + '.' + name,
          .name = name,
          .cpp = cpp,
          .documentation = {},
          .source = {},
          .members = {},
          .value_cpp = std::nullopt,
          .enumerators = {},
      };
      for (const auto* value : enumeration.enumerators()) {
        output.enumerators.emplace_back(value->getNameAsString(),
                                        cpp + "::" + value->getNameAsString());
      }
      collection_->module.classes.push_back(std::move(output));
    }
    Type result{
        .layouts = {},
        .value = layout::Type{.kind = layout::Type::Kind::Enum},
        .cpp = Codec(cpp) + "::Argument",
    };
    layout::Nominal nominal{
        .module = PathText(Utf8Path(collection_->output_directory) /
                           (collection_->module.name + ".vela")),
        .name = name,
        .arguments = {},
        .exported = true,
    };
    const auto identity = layout::NominalKey(result.layouts, nominal);
    result.layouts.nominals.emplace(identity, std::move(nominal));
    result.layouts.classes.push_back({
        .name = identity,
        .native_key = {},
        .native_contract = {},
        .kind = layout::Class::Kind::Enum,
        .reference_fields = {},
        .callable = {},
        .interfaces = {},
        .dispatch_targets = {},
    });
    return result;
  }

  static void AddConstructor(Class& output) {
    Member member{
        .kind = MemberKind::Constructor,
        .name = "constructor",
        .key = NativeMemberKey(output.key, MemberKind::Constructor, "constructor"),
        .cpp_name = {},
        .cpp_pointer_type = {},
        .documentation = {},
        .parameters = {},
        .result = HostType(output.key, Ownership::Local),
        .source = {},
    };
    member.implementation =
        "auto $name(::vela::runtime::NativeContext& context) noexcept { return "
        "context.Construct<::" +
        output.cpp + ">(std::in_place); }\n";
    output.members.push_back(std::move(member));
  }

  void AddField(Class& output, const clang::FieldDecl& field) {
    const auto name = field.getNameAsString();
    if (!frontend::IsMemberIdentifier(name)) {
      Error(field, "native field is not a Vela member identifier: " + name);
      return;
    }
    auto mapped = Map(field.getType());
    const auto codec = Codec(Cpp(field.getType()));
    const bool callback = mapped.value && mapped.value->kind == layout::Type::Kind::Callable;
    if (!callback) {
      Member getter{
          .kind = MemberKind::Getter,
          .name = name,
          .key = NativeMemberKey(output.key, MemberKind::Getter, name),
          .cpp_name = {},
          .cpp_pointer_type = {},
          .documentation = {},
          .parameters = {},
          .result = mapped,
          .source = {},
          .context = true,
          .fallible = true,
          .is_const = true,
      };
      getter.implementation =
          "::vela::Result<" + codec +
          "::Output> $name("
          "::vela::runtime::NativeContext& context, const ::" +
          output.cpp +
          "& receiver) noexcept {"
          "if (!receiver.value) return std::unexpected(::vela::bindings::ValueError(\"Native value "
          "has already been consumed\"));"
          "return " +
          codec + "::Encode(context, receiver.value->" + name + ");}\n";
      output.members.push_back(std::move(getter));
    }
    if (!field.getType().isConstQualified()) {
      Member setter{
          .kind = MemberKind::Setter,
          .name = name,
          .key = NativeMemberKey(output.key, MemberKind::Setter, name),
          .cpp_name = {},
          .cpp_pointer_type = {},
          .documentation = {},
          .parameters = {{.name = "value", .type = mapped, .source = {}}},
          .result = {},
          .source = {},
          .context = true,
          .fallible = true,
      };
      setter.implementation =
          "::vela::Result<void> $name(::vela::runtime::NativeContext& context, ::" + output.cpp +
          "& receiver, " + mapped.cpp +
          " value) noexcept {"
          "if (!receiver.value) return std::unexpected(::vela::bindings::ValueError(\"Native value "
          "has already been consumed\"));"
          "auto decoded = " +
          codec +
          "::Decode(context, value);"
          "if (!decoded) return std::unexpected(std::move(decoded.error()));"
          "receiver.value->" +
          name + " = std::move(*decoded); return {}; }\n";
      output.members.push_back(std::move(setter));
    }
  }

  void CollectFunction(const clang::VarDecl& variable,
                       const clang::FunctionDecl& function,
                       Class& output) {
    root_ = &variable;
    const auto name = variable.getNameAsString();
    if (!frontend::IsMemberIdentifier(name)) {
      Error(variable, "export requires a Vela member identifier");
      return;
    }
    auto result_type = function.getReturnType();
    const auto expected = UnwrapExpected(result_type);
    const bool fallible = !expected.isNull();
    if (fallible) {
      result_type = expected;
    }
    Member member{
        .kind = MemberKind::Static,
        .name = name,
        .key = NativeMemberKey(output.key, MemberKind::Static, name),
        .cpp_name = variable.getQualifiedNameAsString(),
        .cpp_pointer_type = {},
        .documentation = {},
        .parameters = {},
        .result = Map(result_type),
        .source = {},
        .context = true,
        .fallible = true,
    };
    const auto result_cpp = Cpp(result_type);
    const bool is_void = result_type->isVoidType();
    std::ostringstream signature;
    signature << "::vela::Result<" << (is_void ? "void" : Codec(result_cpp) + "::Output")
              << "> $name(::vela::runtime::NativeContext& context";
    std::ostringstream body;
    std::string arguments;
    std::size_t index = 0;
    for (const auto* parameter : function.parameters()) {
      if (index == 0 && IsContext(parameter->getType())) {
        arguments = "context";
        continue;
      }
      auto mapped = Map(parameter->getType());
      auto parameter_name = parameter->getNameAsString();
      if (!frontend::IsDeclarationIdentifier(parameter_name)) {
        parameter_name = "argument" + std::to_string(index);
      }
      member.parameters.push_back({.name = parameter_name, .type = mapped, .source = {}});
      const auto argument = "arg" + std::to_string(index++);
      signature << ", " << mapped.cpp << ' ' << argument;
      body << "auto " << argument << "Value = " << Codec(Cpp(parameter->getType()))
           << "::Decode(context, " << argument << "); if (!" << argument
           << "Value) return std::unexpected(std::move(" << argument << "Value.error()));\n";
      if (!arguments.empty()) {
        arguments += ", ";
      }
      arguments += "std::move(*" + argument + "Value)";
    }
    signature << ") noexcept {\n" << body.str();
    signature << (is_void && !fallible ? "" : "auto result = ") << "::" << member.cpp_name << '('
              << arguments << ");\n";
    if (fallible) {
      signature << "if (!result) return std::unexpected(std::move(result.error()));\n";
    }
    if (is_void) {
      signature << "return {};";
    } else {
      signature << "return " << Codec(result_cpp) << "::Encode(context, "
                << (fallible ? "std::move(*result)" : "std::move(result)") << ");";
    }
    signature << "}\n";
    member.implementation = signature.str();
    output.members.push_back(std::move(member));
  }

  std::string key_;
  clang::CompilerInstance* compiler_;
  clang::ASTContext* context_;
  Collection* collection_;
  const clang::Decl* root_ = nullptr;
  std::map<std::string, std::string> values_;
  std::set<std::string> enums_;
};

}  // namespace

void CollectValueModule(clang::CXXRecordDecl& declaration,
                        const std::string& key,
                        clang::CompilerInstance& compiler,
                        Collection& collection) {
  ValueCollector(key, compiler, collection).Run(declaration);
}

}  // namespace vela::bindgen
