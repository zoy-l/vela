#include "compiler/bindgen/collector.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/RawCommentList.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/FileEntry.h>
#include <clang/Basic/OperatorKinds.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/Specifiers.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Basic/Version.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Sema/Sema.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/FileSystem.h>

#include "base/native_binding.h"
#include "compiler/bindgen/model.h"
#include "compiler/bindgen/type_mapper.h"
#include "compiler/frontend/syntax/names.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::bindgen {
namespace {

std::string RealPath(llvm::StringRef path, clang::DiagnosticsEngine& diagnostics) {
  llvm::SmallString<0> real;
  if (const auto error = llvm::sys::fs::real_path(path, real)) {
    diagnostics.Report(diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error, "%0"))
        << "cannot resolve binding input: " + path.str() + ": " + error.message();
    return {};
  }
  return std::string(real);
}

class Collector final : public clang::RecursiveASTVisitor<Collector> {
 public:
  Collector(clang::CompilerInstance& compiler, Collection& collection)
      : compiler_(&compiler), collection_(&collection), context_(&compiler.getASTContext()) {}

  bool VisitDecl(clang::Decl* declaration) {
    std::size_t count = 0;
    for (const auto* attribute : declaration->specific_attrs<clang::AnnotateAttr>()) {
      const auto text = attribute->getAnnotation();
      if (!text.starts_with("vela.")) {
        continue;
      }
      ++count;
      const bool valid =
          (llvm::isa<clang::CXXRecordDecl>(declaration) && text.starts_with("vela.class:")) ||
          (llvm::isa<clang::CXXMethodDecl>(declaration) &&
           (text.starts_with("vela.name:") || text.starts_with("vela.getter:") ||
            text.starts_with("vela.setter:") || text == "vela.ignore")) ||
          (llvm::isa<clang::ParmVarDecl>(declaration) && text.starts_with("vela.name:")) ||
          (llvm::isa<clang::FieldDecl, clang::VarDecl>(declaration) && text == "vela.ignore" &&
           declaration->getDeclContext()->isRecord());
      if (!valid) {
        Error(*declaration,
              "invalid Vela annotation location or unknown annotation: " + text.str());
      }
      if (text.starts_with("vela.class:")) {
        if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(declaration);
            record != nullptr && !record->hasDefinition()) {
          Error(*declaration, "host class annotation requires a complete class definition");
        }
      }
    }
    if (count > 1) {
      Error(*declaration, "a declaration may have only one Vela annotation");
    }
    return true;
  }

  bool VisitCXXRecordDecl(clang::CXXRecordDecl* declaration) {
    if (!declaration->isThisDeclarationADefinition()) {
      return true;
    }
    const auto key = Annotation(*declaration, "vela.class:");
    if (!key) {
      return true;
    }
    const auto cpp = declaration->getQualifiedNameAsString();
    const auto dependency = collection_->dependencies.find(cpp);
    const bool root = collection_->headers.contains(Location(*declaration).path);
    if (!root && dependency == collection_->dependencies.end()) {
      return true;
    }
    if (root && dependency != collection_->dependencies.end()) {
      Error(*declaration, "root host class already belongs to a binding dependency");
      return true;
    }
    if (!frontend::IsNativeTypeKey(*key)) {
      Error(*declaration, "host type key must contain non-keyword Vela identifiers");
      return true;
    }
    if (dependency != collection_->dependencies.end() && dependency->second != *key) {
      Error(*declaration, "dependency manifest disagrees with the annotated host type key");
      return true;
    }
    const auto [existing, inserted] = keys_.emplace(*key, declaration->getCanonicalDecl());
    if (!inserted && existing->second != declaration->getCanonicalDecl()) {
      Error(*declaration, "duplicate host type key: " + *key);
      return true;
    }
    hosts_[declaration->getCanonicalDecl()] = *key;
    if (root) {
      records_.push_back(declaration);
    }
    return true;
  }

  void Collect() {
    TraverseDecl(context_->getTranslationUnitDecl());
    for (auto* record : records_) {
      CollectClass(*record);
    }
    auto& module = collection_->module;
    module.target = context_->getTargetInfo().getTriple().str();
    module.clang_version = CLANG_VERSION_STRING;
    const auto& sources = context_->getSourceManager();
    std::vector<std::string> parsed_inputs;
    for (auto file = sources.fileinfo_begin(); file != sources.fileinfo_end(); ++file) {
      parsed_inputs.push_back(RealPath(file->first.getName(), compiler_->getDiagnostics()));
    }
    std::ranges::sort(parsed_inputs);
    std::ranges::sort(module.classes, {}, &Class::key);
    for (const auto& header : collection_->headers) {
      if (!std::ranges::binary_search(parsed_inputs, header)) {
        auto& diagnostics = compiler_->getDiagnostics();
        diagnostics.Report(diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error, "%0"))
            << "configured header was not read by the translation unit: " + header;
      }
    }
  }

 private:
  void Error(const clang::Decl& declaration, const std::string& message) {
    auto& diagnostics = compiler_->getDiagnostics();
    diagnostics.Report(declaration.getLocation(),
                       diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error, "%0"))
        << message;
  }

  static std::optional<std::string> Annotation(const clang::Decl& declaration,
                                               llvm::StringRef prefix) {
    for (const auto* attribute : declaration.specific_attrs<clang::AnnotateAttr>()) {
      const auto text = attribute->getAnnotation();
      if (text.starts_with(prefix)) {
        return text.drop_front(prefix.size()).str();
      }
    }
    return std::nullopt;
  }

  Source Location(const clang::Decl& declaration) {
    const auto& sources = context_->getSourceManager();
    const auto location = sources.getExpansionLoc(declaration.getLocation());
    const auto filename = sources.getFilename(location);
    const auto [path, inserted] =
        source_paths_.try_emplace(std::string_view(filename.data(), filename.size()));
    if (inserted) {
      path->second = RealPath(filename, compiler_->getDiagnostics());
    }
    return {
        .path = path->second,
        .line = sources.getExpansionLineNumber(location),
        .column = sources.getExpansionColumnNumber(location),
    };
  }

  std::string Documentation(const clang::Decl& declaration) {
    const auto* comment = context_->getRawCommentForDeclNoCache(&declaration);
    return comment != nullptr ? comment->getFormattedText(context_->getSourceManager(),
                                                          compiler_->getDiagnostics())
                              : std::string{};
  }

  bool IsNothrow(const clang::FunctionDecl& declaration) {
    const auto* prototype = declaration.getType()->getAs<clang::FunctionProtoType>();
    const auto* resolved =
        compiler_->getSema().ResolveExceptionSpec(declaration.getLocation(), prototype);
    return resolved != nullptr && resolved->isNothrow();
  }

  void CollectClass(clang::CXXRecordDecl& record) {
    if (record.isDependentContext() || llvm::isa<clang::ClassTemplateSpecializationDecl>(record) ||
        (record.isLocalClass() != nullptr) || record.isAnonymousStructOrUnion() ||
        record.isUnion() || record.getAccess() == clang::AS_private ||
        record.getAccess() == clang::AS_protected) {
      Error(record, "host classes must be accessible, named, non-template class definitions");
      return;
    }
    auto* destructor = compiler_->getSema().LookupDestructor(&record);
    if (destructor == nullptr || destructor->isDeleted() ||
        destructor->getAccess() != clang::AS_public || !IsNothrow(*destructor)) {
      Error(record, "host class requires an accessible noexcept destructor");
      return;
    }
    const auto& key = hosts_.at(record.getCanonicalDecl());
    const auto export_name = key.substr(key.rfind('.') + 1);
    if (!record.isExternallyVisible() ||
        std::ranges::find(frontend::kBuiltinTypeNames, export_name) !=
            frontend::kBuiltinTypeNames.end()) {
      Error(record, "host class must be externally visible and have a non-builtin export name");
      return;
    }
    for (const auto* parent = record.getDeclContext(); !parent->isTranslationUnit();
         parent = parent->getParent()) {
      if (const auto* outer = llvm::dyn_cast<clang::CXXRecordDecl>(parent);
          outer != nullptr &&
          (outer->getAccess() == clang::AS_private || outer->getAccess() == clang::AS_protected)) {
        Error(record, "host class has an inaccessible enclosing class");
        return;
      }
    }
    if (!export_names_.insert(export_name).second) {
      Error(record, "duplicate module export name: " + export_name);
      return;
    }
    Class output{
        .key = key,
        .name = export_name,
        .cpp = record.getQualifiedNameAsString(),
        .documentation = Documentation(record),
        .source = Location(record),
        .members = {},
    };
    for (auto* declaration : record.decls()) {
      if (declaration->isImplicit() || declaration->getAccess() != clang::AS_public ||
          Annotation(*declaration, "vela.ignore")) {
        continue;
      }
      if (const auto* member_template = llvm::dyn_cast<clang::FunctionTemplateDecl>(declaration);
          member_template != nullptr &&
          Annotation(*member_template->getTemplatedDecl(), "vela.ignore")) {
        continue;
      }
      if (auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(declaration)) {
        CollectMethod(*method, output);
      } else if (llvm::isa<clang::FieldDecl, clang::VarDecl, clang::FunctionTemplateDecl>(
                     declaration)) {
        Error(*declaration, "public fields and member templates require VELA_IGNORE");
      }
    }
    ValidateMembers(record, output);
    collection_->module.classes.push_back(std::move(output));
  }

  bool ValidateConstructor(const clang::CXXConstructorDecl& constructor) {
    const auto& owner = *constructor.getParent();
    if (owner.isAbstract()) {
      Error(constructor, "abstract host classes cannot export a constructor");
      return false;
    }
    std::vector<const clang::CXXRecordDecl*> pending{&owner};
    std::set<const clang::CXXRecordDecl*> visited;
    while (!pending.empty()) {
      const auto* record = pending.back();
      pending.pop_back();
      if (!visited.insert(record).second) {
        continue;
      }
      if (HasClassAllocation(*record)) {
        Error(constructor, "host constructors require global allocation; expose a factory instead");
        return false;
      }
      for (const auto& base : record->bases()) {
        pending.push_back(base.getType()->getAsCXXRecordDecl());
      }
    }
    return true;
  }

  static bool HasClassAllocation(const clang::CXXRecordDecl& record) {
    return std::ranges::any_of(record.methods(), [](const auto* method) {
      switch (method->getOverloadedOperator()) {
        case clang::OO_New:
        case clang::OO_Array_New:
        case clang::OO_Delete:
        case clang::OO_Array_Delete:
          return true;
        default:
          return false;
      }
    });
  }

  bool SkipLifecycle(const clang::CXXMethodDecl& method) {
    const auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(&method);
    if (llvm::isa<clang::CXXDestructorDecl>(method) || method.isCopyAssignmentOperator() ||
        method.isMoveAssignmentOperator() ||
        (constructor != nullptr && constructor->isCopyOrMoveConstructor())) {
      for (const auto* attribute : method.specific_attrs<clang::AnnotateAttr>()) {
        if (attribute->getAnnotation().starts_with("vela.")) {
          Error(method, "lifecycle methods cannot be renamed or exported");
        }
      }
      return true;
    }
    return false;
  }

  void CollectMethod(clang::CXXMethodDecl& method, Class& output) {
    if (SkipLifecycle(method)) {
      return;
    }
    const auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(&method);
    if (method.isImmediateFunction() || method.isExplicitObjectMemberFunction()) {
      Error(method,
            "export requires a runtime-callable method without an explicit object parameter; "
            "use VELA_IGNORE to exclude it");
      return;
    }
    if (method.isDeleted() || method.isPureVirtual() || method.isOverloadedOperator() ||
        llvm::isa<clang::CXXConversionDecl>(method) || method.isVariadic() ||
        method.getRefQualifier() != clang::RQ_None || method.isVolatile() || !IsNothrow(method)) {
      Error(method,
            "export requires a non-deleted noexcept method without operators, varargs, "
            "pure virtual, volatile or ref qualifiers; use VELA_IGNORE to exclude it");
      return;
    }
    Member member{
        .kind = method.isStatic() ? MemberKind::Static : MemberKind::Method,
        .name = method.getNameAsString(),
        .key = {},
        .cpp_name = method.getQualifiedNameAsString(),
        .cpp_pointer_type = {},
        .documentation = Documentation(method),
        .parameters = {},
        .result = {},
        .source = Location(method),
        .is_const = method.isConst(),
    };
    if (constructor != nullptr) {
      if (!ValidateConstructor(*constructor)) {
        return;
      }
      member.kind = MemberKind::Constructor;
    }
    const auto explicit_name = Annotation(method, "vela.name:");
    if (explicit_name) {
      member.name = *explicit_name;
    }
    if (const auto name = Annotation(method, "vela.getter:")) {
      member.name = *name;
      member.kind = MemberKind::Getter;
    }
    if (const auto name = Annotation(method, "vela.setter:")) {
      member.name = *name;
      member.kind = MemberKind::Setter;
    }
    if ((constructor != nullptr && member.kind != MemberKind::Constructor) ||
        (method.isStatic() && member.kind != MemberKind::Static) ||
        (constructor != nullptr && explicit_name)) {
      Error(method, "constructor and static method annotations do not define instance properties");
      return;
    }
    if (constructor != nullptr) {
      member.name = "constructor";
      member.result = HostType(output.key, Ownership::Unique);
      member.result.cpp = output.cpp;
    } else if (!frontend::IsMemberIdentifier(member.name)) {
      Error(method, "exported member name must be a non-keyword Vela identifier");
      return;
    } else {
      CollectResult(method, member);
    }
    CollectParameters(method, member);
    ValidateProperty(method, member);
    member.key = NativeMemberKey(output.key, member.kind, member.name);
    output.members.push_back(std::move(member));
  }

  void ValidateProperty(const clang::CXXMethodDecl& method, const Member& member) {
    if ((member.kind == MemberKind::Getter &&
         (!member.parameters.empty() || member.result.IsVoid())) ||
        (member.kind == MemberKind::Setter &&
         (member.parameters.size() != 1 || !member.result.IsVoid()))) {
      Error(method,
            "getter requires no script arguments and a value; setter requires one "
            "script argument and void result");
    }
  }

  void CollectResult(const clang::CXXMethodDecl& method, Member& member) {
    auto result_type = method.getReturnType();
    const auto payload = UnwrapExpected(result_type);
    member.fallible = !payload.isNull();
    if (member.fallible) {
      result_type = payload;
    }
    const auto result = MapType(result_type, TypePosition::Result, *context_, hosts_);
    if (!result) {
      Error(method, "unsupported result type: " + result.error());
      return;
    }
    member.result = *result;
    const auto pointer = method.isStatic() ? context_->getPointerType(method.getType())
                                           : context_->getMemberPointerType(
                                                 method.getType(), nullptr, method.getParent());
    member.cpp_pointer_type = PrintType(pointer, *context_);
  }

  void CollectParameters(const clang::CXXMethodDecl& method, Member& member) {
    std::set<std::string> names;
    for (const auto* parameter : method.parameters()) {
      if (parameter->hasDefaultArg()) {
        Error(*parameter, "default arguments are not supported by automatic bindings");
      }
      const auto type = parameter->getType();
      if (parameter->getFunctionScopeIndex() == 0 && IsContext(type)) {
        member.context = true;
        continue;
      }
      const auto name = Annotation(*parameter, "vela.name:").value_or(parameter->getNameAsString());
      if (!frontend::IsDeclarationIdentifier(name) || !names.insert(name).second) {
        Error(*parameter, "parameter requires a unique non-keyword Vela name; use VELA_NAME");
      }
      const auto mapped = MapType(type, TypePosition::Parameter, *context_, hosts_);
      if (!mapped) {
        Error(*parameter,
              "unsupported parameter type or missing binding dependency: " + mapped.error());
        continue;
      }
      member.parameters.push_back({.name = name, .type = *mapped, .source = Location(*parameter)});
    }
  }

  void ValidateMembers(const clang::CXXRecordDecl& record, Class& output) {
    std::ranges::sort(output.members, {}, &Member::key);
    std::map<std::string, const Member*> names;
    std::size_t constructors = 0;
    for (const auto& member : output.members) {
      if (member.kind == MemberKind::Constructor) {
        ++constructors;
        continue;
      }
      const auto [existing, inserted] = names.emplace(member.name, &member);
      if (inserted) {
        continue;
      }
      const auto& previous = *existing->second;
      const auto* getter = member.kind == MemberKind::Getter ? &member : &previous;
      const auto* setter = member.kind == MemberKind::Setter ? &member : &previous;
      if (getter->kind != MemberKind::Getter || setter->kind != MemberKind::Setter ||
          setter->parameters.size() != 1 ||
          !getter->result.IsSameLogicalType(setter->parameters[0].type)) {
        Error(record, "conflicting exported member or property types: " + member.name +
                          "; rename overloads with VELA_NAME or exclude with VELA_IGNORE");
      }
    }
    if (constructors > 1) {
      Error(record, "at most one public constructor may be exported; use VELA_IGNORE");
    }
  }

  clang::CompilerInstance* compiler_;
  Collection* collection_;
  clang::ASTContext* context_;
  HostTypes hosts_;
  std::map<std::string, const clang::CXXRecordDecl*> keys_;
  std::set<std::string> export_names_;
  std::vector<clang::CXXRecordDecl*> records_;
  absl::flat_hash_map<std::string, std::string> source_paths_;
};

class Consumer final : public clang::ASTConsumer {
 public:
  Consumer(clang::CompilerInstance& compiler, Collection& collection)
      : compiler_(&compiler), collection_(&collection) {}

  void HandleTranslationUnit(clang::ASTContext& /*unused*/) override {
    if (!compiler_->getDiagnostics().hasErrorOccurred()) {
      Collector(*compiler_, *collection_).Collect();
    }
  }

 private:
  clang::CompilerInstance* compiler_;
  Collection* collection_;
};

class Action final : public clang::ASTFrontendAction {
 public:
  explicit Action(Collection& collection) : collection_(&collection) {}

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& compiler,
                                                        llvm::StringRef /*unused*/) override {
    return std::make_unique<Consumer>(compiler, *collection_);
  }

 private:
  Collection* collection_;
};

}  // namespace

std::unique_ptr<clang::FrontendAction> CollectorFactory::create() {
  return std::make_unique<Action>(*collection_);
}

}  // namespace vela::bindgen
