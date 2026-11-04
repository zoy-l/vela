#include "runtime/isolate/isolate.h"

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/native_binding.h"
#include "base/utf8.h"
#include "runtime/native/native_class.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_type.h"
#include "runtime/value/object.h"

namespace vela::runtime {
namespace {

using Classes = absl::flat_hash_map<std::string, NativeClassBinding>;
using Registration = std::expected<void, NativeRegistrationError>;

NativeContractType ContractType(NativeType type) {
  std::string_view key;
  switch (type.kind) {
    case NativeKind::Void:
      key = "void";
      break;
    case NativeKind::Boolean:
      key = "bool";
      break;
    case NativeKind::Integer:
      key = "int";
      break;
    case NativeKind::Double:
      key = "double";
      break;
    case NativeKind::String:
      key = "string";
      break;
    case NativeKind::Future:
      key = "Future";
      break;
    case NativeKind::CancellationToken:
      key = "CancellationToken";
      break;
    case NativeKind::Host:
      key = type.host_key;
      break;
  }
  return {.key = std::string(key), .nullable = type.nullable};
}

std::string Contract(const NativeClass& type) {
  std::vector<NativeContractMember> members;
  for (const auto& source : type.members) {
    NativeContractMember member{
        .key = std::string(source.function.name()),
        .parameters = {},
        .result = ContractType(source.function.result_type()),
    };
    const auto offset = static_cast<std::size_t>(HasNativeReceiver(source.kind));
    const auto parameters = source.function.parameters().subspan(offset);
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      member.parameters.push_back(
          {.name = source.parameter_names[index], .type = ContractType(parameters[index])});
    }
    members.push_back(std::move(member));
  }
  return NativeClassContract(std::move(members)).Hex();
}

bool IsName(std::string_view name) {
  return !name.empty() && !name.contains('\0') && !InvalidUtf8Offset(name);
}

Registration ValidateType(NativeType type, const Classes& classes) {
  if (type.kind != NativeKind::Host) {
    return {};
  }
  const auto found = classes.find(type.host_key);
  if (found == classes.end()) {
    return std::unexpected(NativeRegistrationError::UnknownClass);
  }
  if (type.cpp_identity != found->second.description->type.cpp_identity) {
    return std::unexpected(NativeRegistrationError::TypeMismatch);
  }
  return {};
}

Registration ValidateSignature(const NativeFunction& function, const Classes& classes) {
  if (auto result = ValidateType(function.result_type(), classes); !result) {
    return result;
  }
  for (const auto type : function.parameters()) {
    if (auto result = ValidateType(type, classes); !result) {
      return result;
    }
  }
  return {};
}

Registration ValidateMember(const NativeClass& owner, const NativeClassMember& member) {
  const auto& function = member.function;
  const auto parameters = function.parameters();
  const auto result = function.result_type();
  const auto receiver_count = static_cast<std::size_t>(HasNativeReceiver(member.kind));
  if (!IsName(member.name) ||
      function.name() != NativeMemberKey(owner.type.host_key, member.kind, member.name) ||
      parameters.size() != member.parameter_names.size() + receiver_count) {
    return std::unexpected(NativeRegistrationError::InvalidMember);
  }
  if (receiver_count != 0 && parameters.front() != owner.type) {
    return std::unexpected(NativeRegistrationError::TypeMismatch);
  }
  if (member.kind == NativeMemberKind::Constructor && result != owner.type) {
    return std::unexpected(NativeRegistrationError::TypeMismatch);
  }
  absl::flat_hash_set<std::string_view> names;
  for (const auto& name : member.parameter_names) {
    if (!IsName(name) || !names.insert(name).second) {
      return std::unexpected(NativeRegistrationError::InvalidMember);
    }
  }
  if ((member.kind == NativeMemberKind::Getter &&
       (!member.parameter_names.empty() || result.kind == NativeKind::Void)) ||
      (member.kind == NativeMemberKind::Setter &&
       (member.parameter_names.size() != 1 || result.kind != NativeKind::Void))) {
    return std::unexpected(NativeRegistrationError::InvalidMember);
  }
  return {};
}

Registration ValidateMembers(const NativeClass& owner, const Classes& classes) {
  absl::flat_hash_map<std::string, const NativeClassMember*> names;
  for (const auto& member : owner.members) {
    if (auto result = ValidateMember(owner, member); !result) {
      return result;
    }
    if (auto result = ValidateSignature(member.function, classes); !result) {
      return result;
    }
    const auto [existing, inserted] = names.emplace(member.name, &member);
    if (inserted) {
      continue;
    }
    const auto& previous = *existing->second;
    const auto& getter = member.kind == NativeMemberKind::Getter ? member : previous;
    const auto& setter = member.kind == NativeMemberKind::Setter ? member : previous;
    if (getter.kind != NativeMemberKind::Getter || setter.kind != NativeMemberKind::Setter ||
        getter.function.result_type() != setter.function.parameters().back()) {
      return std::unexpected(NativeRegistrationError::DuplicateName);
    }
  }
  return {};
}

}  // namespace

std::expected<void, NativeRegistrationError> Isolate::ValidateNativeSignature(
    const NativeFunction& function) const {
  return ValidateSignature(function, native_classes_);
}

const NativeClassBinding* Isolate::FindNativeClass(std::string_view key) const noexcept {
  const auto found = native_classes_.find(key);
  return found == native_classes_.end() ? nullptr : &found->second;
}

ClassLayout* Isolate::CreateNativeClassLayout() {
  auto* nominal =
      CreateNominalType({.kind = NominalKind::Class, .interfaces = {}, .declaration = {}});
  ClassLayoutOptions options;
  options.nominal = nominal;
  options.storage = ClassStorage::Host;
  return CreateClassLayout(0, {}, std::move(options));
}

std::expected<void, NativeRegistrationError> Isolate::RegisterNativeModule(
    std::vector<NativeClass> classes) {
  if (bindings_frozen_) {
    return std::unexpected(NativeRegistrationError::BindingsFrozen);
  }
  auto staged_classes = native_classes_;
  auto staged_functions = natives_;
  std::vector<std::shared_ptr<NativeClass>> module;
  module.reserve(classes.size());
  for (auto& type : classes) {
    const auto descriptor = type.type;
    const auto key = descriptor.host_key;
    if (!IsName(key) || descriptor.kind != NativeKind::Host || descriptor.nullable ||
        descriptor.cpp_identity == nullptr || type.size == 0 || type.alignment == 0) {
      return std::unexpected(NativeRegistrationError::InvalidClass);
    }
    auto owner = std::make_shared<NativeClass>(std::move(type));
    for (auto& member : owner->members) {
      member.function.member_ = &member;
    }
    if (!staged_classes
             .emplace(std::string(key),
                      NativeClassBinding{
                          .description = owner,
                      })
             .second) {
      return std::unexpected(NativeRegistrationError::DuplicateName);
    }
    module.push_back(std::move(owner));
  }
  for (const auto& owner : module) {
    if (auto result = ValidateMembers(*owner, staged_classes); !result) {
      return result;
    }
    const auto contract = Contract(*owner);
    if (!owner->contract.empty() && owner->contract != contract) {
      return std::unexpected(NativeRegistrationError::TypeMismatch);
    }
    owner->contract = contract;
    for (const auto& member : owner->members) {
      auto function = std::shared_ptr<const NativeFunction>(owner, &member.function);
      if (!staged_functions.emplace(std::string(member.function.name()), std::move(function))
               .second) {
        return std::unexpected(NativeRegistrationError::DuplicateName);
      }
    }
  }
  for (const auto& owner : module) {
    staged_classes.at(owner->type.host_key).layout = CreateNativeClassLayout();
  }
  natives_.swap(staged_functions);
  native_classes_.swap(staged_classes);
  return {};
}

}  // namespace vela::runtime
