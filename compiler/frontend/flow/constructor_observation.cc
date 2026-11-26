#include "compiler/frontend/flow/constructor_observation.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/names.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"

namespace vela::frontend {
namespace {

bool IsReceiver(const Expression& expression) {
  return expression.kind == Expression::Kind::Name &&
         (expression.text == kReceiverName || expression.text == kSuperName);
}

class Observation final {
 public:
  Observation(const ClassDescriptor& receiver,
              const absl::node_hash_map<std::string, ClassDescriptor>& classes,
              const absl::flat_hash_map<std::string_view, const Class*>& definitions)
      : receiver_(&receiver), classes_(&classes), definitions_(&definitions) {}

  ConstructorObservation Run(const ClassDescriptor& base) {
    Constructor(base);
    return std::move(result_);
  }

 private:
  enum class Access : unsigned char { Method, Getter, Setter, ReadWrite };
  enum class Dispatch : unsigned char { Virtual, Lexical };
  void Constructor(const ClassDescriptor& owner);
  void Method(const ClassDescriptor& owner,
              std::string_view name,
              Access access,
              Dispatch dispatch);
  void FunctionBody(const ClassDescriptor& owner, const Function& function);
  void Walk(const ClassDescriptor& owner, const Statement* statement);
  void Walk(const ClassDescriptor& owner, const Expression* expression);
  void Member(const ClassDescriptor& owner, const Expression& member, Access access);
  bool WalkCall(const ClassDescriptor& owner, const Expression& expression);
  bool WalkSpecial(const ClassDescriptor& owner, const Expression& expression);
  void IndexMethods(const ClassDescriptor& owner);

  const ClassDescriptor* receiver_;
  const absl::node_hash_map<std::string, ClassDescriptor>* classes_;
  const absl::flat_hash_map<std::string_view, const Class*>* definitions_;
  ConstructorObservation result_;
  absl::flat_hash_set<std::string_view> constructors_;
  absl::flat_hash_set<std::string_view> indexed_;
  absl::flat_hash_set<const Statement*> visited_;
  absl::flat_hash_map<std::tuple<std::string_view, std::string_view, Access>, const Function*>
      methods_;
  std::uint32_t capture_depth_ = 0;
};

void Observation::IndexMethods(const ClassDescriptor& owner) {
  if (!indexed_.insert(owner.type.name).second) {
    return;
  }
  const auto& definition = *definitions_->at(owner.type.name);
  for (const auto& method : definition.methods) {
    auto access = Access::Method;
    if (method.is_getter) {
      access = Access::Getter;
    } else if (method.is_setter) {
      access = Access::Setter;
    }
    methods_.emplace(std::tuple(owner.type.name, std::string_view(method.name), access), &method);
  }
}

void Observation::FunctionBody(const ClassDescriptor& owner, const Function& function) {
  if (function.body == nullptr) {
    result_.escapes_receiver = true;
    return;
  }
  if (!visited_.insert(function.body.get()).second) {
    return;
  }
  VisitFunctionChildren(
      function, [&](const Expression* child) { Walk(owner, child); },
      [&](const Statement* child) { Walk(owner, child); });
}

void Observation::Constructor(const ClassDescriptor& owner) {
  if (!constructors_.insert(owner.type.name).second) {
    return;
  }
  const auto& source = *definitions_->at(owner.type.name);
  if (owner.base_class && !source.HasExplicitSuperCall()) {
    Constructor(classes_->at(*owner.base_class));
  }
  if (source.constructor) {
    FunctionBody(owner, *source.constructor);
  }
}

void Observation::Method(const ClassDescriptor& owner,
                         std::string_view name,
                         Access access,
                         Dispatch dispatch) {
  const auto select = [&](const auto& methods, const auto* current, Visibility visibility) {
    if (dispatch == Dispatch::Virtual && visibility == Visibility::Public) {
      const auto implementation = methods.find(name);
      if (implementation != methods.end()) {
        return &implementation->second;
      }
    }
    return current;
  };
  const auto found = owner.methods.find(name);
  if (found == owner.methods.end()) {
    if (access != Access::Method) {
      result_.escapes_receiver = true;
      return;
    }
    const auto generic = owner.method_templates.find(name);
    if (generic == owner.method_templates.end()) {
      result_.escapes_receiver = true;
      return;
    }
    const auto* target = select(receiver_->method_templates, &generic->second,
                                generic->second.declaration->visibility);
    FunctionBody(classes_->at(target->owner), *target->declaration);
    return;
  }
  const auto* signature = &found->second;
  if (access == Access::Getter && !signature->is_getter) {
    result_.escapes_receiver = true;
    return;
  }
  const auto visibility =
      access == Access::Setter ? signature->setter_visibility : signature->visibility;
  signature = select(receiver_->methods, signature, visibility);
  const auto& declaring =
      access == Access::Setter ? signature->setter_owner : signature->member_owner;
  const auto target = classes_->find(declaring);
  if (target == classes_->end()) {
    result_.escapes_receiver = true;
    return;
  }
  IndexMethods(target->second);
  const auto method = methods_.find(std::tuple(target->second.type.name, name, access));
  if (method == methods_.end()) {
    result_.escapes_receiver = true;
    return;
  }
  FunctionBody(target->second, *method->second);
}

void Observation::Member(const ClassDescriptor& owner, const Expression& member, Access access) {
  if (capture_depth_ != 0) {
    result_.escapes_receiver = true;
    return;
  }
  const auto lexical = member.object->text == kSuperName;
  const auto* target = &owner;
  if (lexical) {
    if (!owner.base_class) {
      result_.escapes_receiver = true;
      return;
    }
    target = &classes_->at(*owner.base_class);
  }
  if (const auto* field = target->FindField(member.text); field != nullptr) {
    result_.fields.insert(field->index);
    return;
  }
  const auto dispatch = lexical ? Dispatch::Lexical : Dispatch::Virtual;
  if (access == Access::ReadWrite) {
    Method(*target, member.text, Access::Getter, dispatch);
    Method(*target, member.text, Access::Setter, dispatch);
  } else {
    Method(*target, member.text, access, dispatch);
  }
}

bool Observation::WalkCall(const ClassDescriptor& owner, const Expression& expression) {
  const auto& callee = *expression.callee;
  if (callee.kind == Expression::Kind::Name && callee.text == kSuperName && owner.base_class) {
    Constructor(classes_->at(*owner.base_class));
  } else if (callee.kind == Expression::Kind::Member && IsReceiver(*callee.object)) {
    Member(owner, callee, Access::Method);
  } else {
    return false;
  }
  for (const auto& argument : expression.arguments) {
    Walk(owner, argument.value.get());
  }
  return true;
}

bool Observation::WalkSpecial(const ClassDescriptor& owner, const Expression& expression) {
  if (expression.kind == Expression::Kind::Name && IsReceiver(expression)) {
    result_.escapes_receiver = true;
    return true;
  }
  if (expression.kind == Expression::Kind::Lambda) {
    // 创建捕获 this 的闭包即暴露实例，不依赖闭包是否在本构造体内调用。
    ++capture_depth_;
    VisitFunctionChildren(
        *expression.function, [&](const Expression* child) { Walk(owner, child); },
        [&](const Statement* child) { Walk(owner, child); });
    --capture_depth_;
    return true;
  }
  if (expression.kind == Expression::Kind::Call) {
    return WalkCall(owner, expression);
  }
  if (expression.kind == Expression::Kind::Member && IsReceiver(*expression.object)) {
    Member(owner, expression, Access::Getter);
    return true;
  }
  if (expression.kind == Expression::Kind::Unary &&
      (expression.operation == TokenKind::PlusPlus ||
       expression.operation == TokenKind::MinusMinus) &&
      expression.operand->kind == Expression::Kind::Member &&
      IsReceiver(*expression.operand->object)) {
    Member(owner, *expression.operand, Access::ReadWrite);
    return true;
  }
  if ((expression.kind == Expression::Kind::Binary && expression.operation == TokenKind::Equal) ||
      expression.kind == Expression::Kind::CompoundAssignment) {
    const auto& left = *expression.left;
    if (left.kind == Expression::Kind::Member && IsReceiver(*left.object)) {
      Member(owner, left,
             expression.kind == Expression::Kind::CompoundAssignment ? Access::ReadWrite
                                                                     : Access::Setter);
      Walk(owner, expression.right.get());
      return true;
    }
  }
  return false;
}

void Observation::Walk(const ClassDescriptor& owner, const Expression* expression) {
  if (expression == nullptr || result_.escapes_receiver || WalkSpecial(owner, *expression)) {
    return;
  }
  VisitExpressionChildren(*expression, [&](const Expression* child) { Walk(owner, child); });
}

void Observation::Walk(const ClassDescriptor& owner, const Statement* statement) {
  if (statement == nullptr || result_.escapes_receiver) {
    return;
  }
  VisitStatementChildren(
      *statement, [&](const Expression* child) { Walk(owner, child); },
      [&](const Statement* child) { Walk(owner, child); });
}

}  // namespace

ConstructorObservation AnalyzeBaseConstruction(
    const ClassDescriptor& receiver,
    const ClassDescriptor& base,
    const absl::node_hash_map<std::string, ClassDescriptor>& classes,
    const absl::flat_hash_map<std::string_view, const Class*>& definitions) {
  return Observation(receiver, classes, definitions).Run(base);
}

}  // namespace vela::frontend
