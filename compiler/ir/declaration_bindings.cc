#include "compiler/ir/declaration_bindings.h"
#include "compiler/frontend/syntax/names.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/declaration.h"
#include "compiler/frontend/flow/lambda_visitor.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"

namespace vela::ir {

std::string InitializerKey(InitializerKind kind, std::string_view name) {
  std::string key;
  switch (kind) {
    case InitializerKind::Global:
      key = "$global.";
      break;
    case InitializerKind::StaticField:
      key = "$static.";
      break;
    case InitializerKind::InstanceField:
      key = "$field.";
      break;
    case InitializerKind::ConstructorFactory:
      key = "$construct.";
      break;
  }
  key += name;
  return key;
}
namespace {

template <class Method>
DeclarationKind DescribeMethodKind(const Method& method) {
  if (method.is_getter) {
    return DeclarationKind::Getter;
  }
  if (method.is_setter) {
    return DeclarationKind::Setter;
  }
  return method.storage == frontend::MemberStorage::Static ? DeclarationKind::StaticMethod
                                                           : DeclarationKind::Method;
}

}  // namespace

DeclarationKind MethodDeclarationKind(const frontend::Function& method) {
  return DescribeMethodKind(method);
}

DeclarationKind MethodDeclarationKind(const frontend::FunctionDescriptor& method) {
  return DescribeMethodKind(method);
}

std::string MethodKey(std::string_view owner, DeclarationKind kind, std::string_view name) {
  constexpr std::string_view kGetterPrefix = "$get.";
  constexpr std::string_view kSetterPrefix = "$set.";
  std::string key(owner);
  key += '.';
  if (kind == DeclarationKind::Getter) {
    key += kGetterPrefix;
  } else if (kind == DeclarationKind::Setter) {
    key += kSetterPrefix;
  }
  key += name;
  return key;
}

std::string MethodKey(std::string_view owner, const frontend::Function& method) {
  return MethodKey(owner, MethodDeclarationKind(method), method.name);
}

namespace {

class DeclarationBinder final {
 public:
  DeclarationBinder(const frontend::Program& source,
                    const frontend::SemanticModel& model,
                    frontend::DeclarationIds& identities,
                    std::uint64_t revision)
      : source_(&source), model_(&model), identities_(&identities) {
    result_.revision = revision;
  }
  DeclarationBindings Run();

 private:
  DeclarationId Member(DeclarationId owner,
                       DeclarationKind kind,
                       const std::string& name,
                       DeclarationId definition = {});
  frontend::TypeIdentity Type(const frontend::TypeName& source);
  DeclarationId Origin(const frontend::DeclarationOrigin& origin);
  DeclarationId Method(DeclarationId owner,
                       DeclarationKind kind,
                       const frontend::Function& method,
                       DeclarationId definition);
  void BindFunction(const frontend::Function& function, DeclarationId identity);
  void BindExpression(const frontend::Expression* expression, DeclarationId identity);
  template <typename Node>
  void RegisterLambdas(const Node& node, DeclarationId identity);
  void BindClass(const frontend::Class& owner);
  void BindInterface(const frontend::Interface& interface);
  void InheritFields();

  const frontend::Program* source_;
  const frontend::SemanticModel* model_;
  frontend::DeclarationIds* identities_;
  DeclarationBindings result_;
  absl::flat_hash_map<std::string, DeclarationId> modules_;
};

DeclarationId DeclarationBinder::Member(DeclarationId owner,
                                        DeclarationKind kind,
                                        const std::string& name,
                                        DeclarationId definition) {
  return identities_->Get({.owner = owner, .kind = kind, .name = name, .arguments = {}},
                          definition);
}

DeclarationId DeclarationBinder::Origin(const frontend::DeclarationOrigin& origin) {
  const auto module = modules_.at(origin.module_uri);
  const auto declaration = Member(module, origin.kind, origin.name);
  if (origin.arguments.empty()) {
    return declaration;
  }
  std::vector<frontend::TypeIdentity> arguments;
  arguments.reserve(origin.arguments.size());
  for (const auto& argument : origin.arguments) {
    arguments.push_back(Type(argument));
  }
  return identities_->Get(
      {
          .owner = declaration,
          .kind = origin.kind,
          .name = origin.name,
          .arguments = std::move(arguments),
      },
      declaration);
}

frontend::TypeIdentity DeclarationBinder::Type(const frontend::TypeName& source) {
  frontend::TypeIdentity result;
  using Kind = frontend::TypeIdentity::Kind;
  switch (source.kind) {
    case frontend::TypeName::Kind::Named:
      result.kind = Kind::Builtin;
      break;
    case frontend::TypeName::Kind::Generic:
      result.kind = Kind::Generic;
      break;
    case frontend::TypeName::Kind::Array:
      result.kind = Kind::Array;
      break;
    case frontend::TypeName::Kind::Nullable:
      result.kind = Kind::Nullable;
      break;
    case frontend::TypeName::Kind::Function:
      result.kind = Kind::Function;
      break;
    case frontend::TypeName::Kind::Inferred:
    case frontend::TypeName::Kind::Parameter:
      // 持久执行类型只来自已验证的具体应用；抽象定义事实不进入此绑定阶段。
      std::unreachable();
  }
  if (source.kind == frontend::TypeName::Kind::Named ||
      source.kind == frontend::TypeName::Kind::Generic) {
    const auto origin = source_->origins.find(source.name);
    const auto checked = model_->types.find(source.name);
    const auto nominal = checked != model_->types.end() &&
                         (checked->second.kind == frontend::TypeDescriptor::Kind::Class ||
                          checked->second.kind == frontend::TypeDescriptor::Kind::Interface ||
                          checked->second.kind == frontend::TypeDescriptor::Kind::Enum);
    if (nominal && origin != source_->origins.end()) {
      result.kind = Kind::Declared;
      result.declaration = Origin(origin->second);
    } else {
      // 已检查的未声明名字是 int、Future 等内建类型，不是运行时字符串查找。
      result.builtin = source.name;
    }
  }
  result.arguments.reserve(source.arguments.size());
  for (const auto& argument : source.arguments) {
    result.arguments.push_back(Type(argument));
  }
  return result;
}

DeclarationId DeclarationBinder::Method(DeclarationId owner,
                                        DeclarationKind kind,
                                        const frontend::Function& method,
                                        DeclarationId definition) {
  const auto& name = method.application ? method.application->name : method.name;
  const auto source =
      definition == DeclarationId{} ? DeclarationId{} : Member(definition, kind, name);
  const auto declaration = Member(owner, kind, name, source);
  if (!method.application) {
    return declaration;
  }
  std::vector<frontend::TypeIdentity> arguments;
  arguments.reserve(method.application->arguments.size());
  for (const auto& argument : method.application->arguments) {
    arguments.push_back(Type(argument));
  }
  return identities_->Get(
      {.owner = declaration, .kind = kind, .name = name, .arguments = std::move(arguments)},
      source == DeclarationId{} ? declaration : source);
}

template <typename Node>
void DeclarationBinder::RegisterLambdas(const Node& node, DeclarationId identity) {
  std::uint32_t local = 0;
  frontend::VisitLambdas(node, [&](const frontend::Expression& expression) {
    result_.functions.emplace(expression.function.get(), FunctionIdentity{
                                                             .declaration = identity,
                                                             .revision = result_.revision,
                                                             .local = ++local,
                                                         });
  });
}

void DeclarationBinder::BindFunction(const frontend::Function& function, DeclarationId identity) {
  result_.functions.emplace(
      &function, FunctionIdentity{.declaration = identity, .revision = result_.revision});
  if (function.body != nullptr) {
    RegisterLambdas(*function.body, identity);
  }
}

void DeclarationBinder::BindExpression(const frontend::Expression* expression,
                                       DeclarationId identity) {
  if (expression == nullptr) {
    return;
  }
  RegisterLambdas(*expression, identity);
}

void DeclarationBinder::BindClass(const frontend::Class& owner) {
  const auto declaration = result_.nominals.at(owner.name);
  const auto& origin = source_->origins.at(owner.name);
  const auto definition = origin.arguments.empty()
                              ? DeclarationId{}
                              : Member(modules_.at(origin.module_uri), origin.kind, origin.name);
  const auto member = [&](DeclarationKind kind, const std::string& name) {
    const auto template_member =
        definition == DeclarationId{} ? DeclarationId{} : Member(definition, kind, name);
    return Member(declaration, kind, name, template_member);
  };
  const auto initializer = [&](DeclarationId field, DeclarationKind kind, const std::string& name) {
    const auto template_initializer =
        definition == DeclarationId{}
            ? DeclarationId{}
            : Member(Member(definition, kind, name), DeclarationKind::Initializer, "");
    return Member(field, DeclarationKind::Initializer, "", template_initializer);
  };
  auto& fields = result_.classes.try_emplace(owner.name).first->second;
  fields.declaration = declaration;
  const auto constructor =
      member(DeclarationKind::Constructor, std::string(frontend::kConstructorName));
  result_.named_functions.emplace(
      MethodKey(owner.name, DeclarationKind::Method, frontend::kConstructorName), constructor);
  if (owner.constructor != nullptr) {
    BindFunction(*owner.constructor, constructor);
  }
  for (const auto& method : owner.methods) {
    const auto kind = MethodDeclarationKind(method);
    const auto identity = Method(declaration, kind, method, definition);
    result_.named_functions.emplace(MethodKey(owner.name, kind, method.name), identity);
    BindFunction(method, identity);
  }
  for (const auto& method : owner.method_templates) {
    const auto kind = MethodDeclarationKind(method);
    static_cast<void>(Method(declaration, kind, method, definition));
  }
  for (const auto& field : owner.fields) {
    const auto is_static = field.storage == frontend::MemberStorage::Static;
    const auto kind = is_static ? DeclarationKind::StaticField : DeclarationKind::Field;
    const auto identity = member(kind, field.name);
    if (is_static) {
      const auto name = owner.name + '.' + field.name;
      result_.globals.emplace(name, identity);
      const auto target = initializer(identity, kind, field.name);
      result_.named_functions.emplace(InitializerKey(InitializerKind::StaticField, name), target);
      BindExpression(field.initializer.get(), target);
    } else {
      fields.fields.emplace(field.name, identity);
      if (field.initializer != nullptr) {
        const auto target = initializer(identity, kind, field.name);
        result_.named_functions.emplace(
            InitializerKey(InitializerKind::InstanceField, owner.name + '.' + field.name), target);
        BindExpression(field.initializer.get(), target);
      }
    }
  }
}

void DeclarationBinder::InheritFields() {
  // 拓扑遍历每条父类边一次；每个具体类只复制它实际包含的继承字段。
  absl::flat_hash_map<std::string, std::vector<std::string>> children;
  std::vector<std::string> pending;
  for (const auto& owner : source_->classes) {
    const auto& parent = model_->classes.at(owner.name).base_class;
    if (parent) {
      children.try_emplace(*parent).first->second.push_back(owner.name);
    } else {
      pending.push_back(owner.name);
    }
  }
  while (!pending.empty()) {
    auto parent = std::move(pending.back());
    pending.pop_back();
    const auto found = children.find(parent);
    if (found == children.end()) {
      continue;
    }
    const auto& inherited = result_.classes.at(parent).fields;
    for (const auto& child : found->second) {
      auto& fields = result_.classes.at(child).fields;
      for (const auto& [name, identity] : inherited) {
        fields.emplace(name, identity);
      }
      pending.push_back(child);
    }
  }
}

void DeclarationBinder::BindInterface(const frontend::Interface& interface) {
  const auto declaration = result_.nominals.at(interface.name);
  const auto& origin = source_->origins.at(interface.name);
  const auto definition = origin.arguments.empty()
                              ? DeclarationId{}
                              : Member(modules_.at(origin.module_uri), origin.kind, origin.name);
  for (const auto& method : interface.methods) {
    result_.selectors.emplace(interface.name + '.' + method.name,
                              Method(declaration, DeclarationKind::Selector, method, definition));
  }
  for (const auto& method : interface.method_templates) {
    static_cast<void>(Method(declaration, DeclarationKind::Selector, method, definition));
  }
}

DeclarationBindings DeclarationBinder::Run() {
  identities_->BeginUpdate();
  std::vector<std::string> modules;
  modules.reserve(source_->sources.size());
  for (const auto& source : source_->sources) {
    modules.push_back(source.path);
  }
  std::ranges::sort(modules);
  for (const auto& module : modules) {
    modules_.emplace(module, Member({}, DeclarationKind::Module, module));
  }
  std::vector<std::string> names;
  names.reserve(source_->origins.size());
  for (const auto& [name, origin] : source_->origins) {
    names.push_back(name);
  }
  std::ranges::sort(names);
  for (const auto& name : names) {
    const auto& origin = source_->origins.at(name);
    const auto identity = Origin(origin);
    if (origin.arguments.empty()) {
      for (const auto& member : origin.members) {
        const auto target = Member(identity, member.kind, member.name);
        if (member.has_initializer || member.kind == DeclarationKind::StaticField) {
          (void)Member(target, DeclarationKind::Initializer, "");
        }
      }
    }
    if (origin.kind == DeclarationKind::Function) {
      result_.named_functions.emplace(name, identity);
    } else if (origin.kind == DeclarationKind::Global) {
      result_.globals.emplace(name, identity);
      result_.named_functions.emplace(InitializerKey(InitializerKind::Global, name),
                                      Member(identity, DeclarationKind::Initializer, ""));
    } else {
      result_.nominals.emplace(name, identity);
    }
  }
  for (const auto& function : source_->functions) {
    BindFunction(function, result_.named_functions.at(function.name));
  }
  for (const auto& owner : source_->classes) {
    BindClass(owner);
  }
  for (const auto& interface : source_->interfaces) {
    BindInterface(interface);
  }
  for (const auto& global : source_->globals) {
    BindExpression(global.initializer.get(), result_.named_functions.at(InitializerKey(
                                                 InitializerKind::Global, global.name)));
  }
  InheritFields();
  identities_->FinishUpdate();
  return std::move(result_);
}

}  // namespace

DeclarationBindings BindDeclarations(const frontend::Program& source,
                                     const frontend::SemanticModel& model,
                                     frontend::DeclarationIds& identities,
                                     std::uint64_t revision) {
  return DeclarationBinder(source, model, identities, revision).Run();
}

}  // namespace vela::ir
