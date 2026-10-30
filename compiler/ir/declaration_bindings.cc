#include "compiler/ir/declaration_bindings.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/declaration.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/declaration_ids.h"
#include "compiler/frontend/lambda_visitor.h"
#include "compiler/frontend/type_checker.h"

namespace vela::ir {
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
  void BindFunction(const frontend::Function& function, DeclarationId identity);
  void BindExpression(const frontend::Expression* expression, DeclarationId identity);
  void BindClass(const frontend::Class& owner);
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

void DeclarationBinder::BindFunction(const frontend::Function& function, DeclarationId identity) {
  result_.functions.emplace(
      &function, FunctionIdentity{.declaration = identity, .revision = result_.revision});
  if (function.body == nullptr) {
    return;
  }
  std::uint32_t local = 0;
  frontend::VisitLambdas(
      *function.body, [&](const frontend::Expression& expression, const frontend::Function*) {
        result_.functions.emplace(expression.function.get(), FunctionIdentity{
                                                                 .declaration = identity,
                                                                 .revision = result_.revision,
                                                                 .local = ++local,
                                                             });
      });
}

void DeclarationBinder::BindExpression(const frontend::Expression* expression,
                                       DeclarationId identity) {
  if (expression == nullptr) {
    return;
  }
  std::uint32_t local = 0;
  frontend::VisitLambdas(
      *expression, [&](const frontend::Expression& lambda, const frontend::Function*) {
        result_.functions.emplace(lambda.function.get(), FunctionIdentity{
                                                             .declaration = identity,
                                                             .revision = result_.revision,
                                                             .local = ++local,
                                                         });
      });
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
  const auto constructor = member(DeclarationKind::Constructor, "$constructor");
  result_.named_functions.emplace(owner.name + ".$constructor", constructor);
  if (owner.constructor != nullptr) {
    BindFunction(*owner.constructor, constructor);
  }
  for (const auto& method : owner.methods) {
    auto kind = method.storage == frontend::MemberStorage::Static ? DeclarationKind::StaticMethod
                                                                  : DeclarationKind::Method;
    std::string key = owner.name + '.';
    if (method.is_getter) {
      kind = DeclarationKind::Getter;
      key += "$get.";
    } else if (method.is_setter) {
      kind = DeclarationKind::Setter;
      key += "$set.";
    }
    key += method.name;
    const auto identity = member(kind, method.name);
    result_.named_functions.emplace(std::move(key), identity);
    BindFunction(method, identity);
  }
  for (const auto& field : owner.fields) {
    const auto is_static = field.storage == frontend::MemberStorage::Static;
    const auto kind = is_static ? DeclarationKind::StaticField : DeclarationKind::Field;
    const auto identity = member(kind, field.name);
    if (is_static) {
      const auto name = owner.name + '.' + field.name;
      result_.globals.emplace(name, identity);
      const auto target = initializer(identity, kind, field.name);
      result_.named_functions.emplace("$static." + name, target);
      BindExpression(field.initializer.get(), target);
    } else {
      fields.fields.emplace(field.name, identity);
      if (field.initializer != nullptr) {
        const auto target = initializer(identity, kind, field.name);
        result_.named_functions.emplace("$field." + owner.name + '.' + field.name, target);
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
      result_.named_functions.emplace("$global." + name,
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
    const auto declaration = result_.nominals.at(interface.name);
    for (const auto& method : interface.methods) {
      result_.selectors.emplace(interface.name + '.' + method.name,
                                Member(declaration, DeclarationKind::Selector, method.name));
    }
  }
  for (const auto& global : source_->globals) {
    BindExpression(global.initializer.get(), result_.named_functions.at("$global." + global.name));
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
