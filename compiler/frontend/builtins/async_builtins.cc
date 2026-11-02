#include "compiler/frontend/builtins/async_builtins.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/async_operation.h"
#include "base/member.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_name.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {
namespace {

using Kind = TypeDescriptor::Kind;
using Storage = MemberStorage;
using Operation = AsyncOperation;

constexpr auto kMembers = std::to_array<AsyncMember>({
    {
        .receiver = Kind::Invalid,
        .storage = Storage::Static,
        .name = kSleepFunctionName,
        .operation = Operation::Count,
        .kind = MemberKind::Method,
        .parameters = {"milliseconds", {}, {}},
        .required = 1,
    },
    {
        .receiver = Kind::Future,
        .storage = Storage::Static,
        .name = "value",
        .operation = Operation::Count,
        .kind = MemberKind::Method,
        .parameters = {"value", {}, {}},
        .required = 1,
    },
    {
        .receiver = Kind::Future,
        .storage = Storage::Static,
        .name = "error",
        .operation = Operation::Count,
        .kind = MemberKind::Method,
        .parameters = {"error", {}, {}},
        .required = 1,
    },
    {
        .receiver = Kind::Future,
        .storage = Storage::Static,
        .name = "sync",
        .operation = Operation::Count,
        .kind = MemberKind::Method,
        .parameters = {"computation", {}, {}},
        .required = 1,
    },
    {
        .receiver = Kind::Future,
        .storage = Storage::Static,
        .name = "microtask",
        .operation = Operation::Microtask,
        .kind = MemberKind::Method,
        .parameters = {"computation", {}, {}},
        .required = 1,
    },
    {
        .receiver = Kind::Future,
        .storage = Storage::Static,
        .name = "delayed",
        .operation = Operation::Delayed,
        .kind = MemberKind::Method,
        .parameters = {"milliseconds", "computation", {}},
        .required = 2,
    },
    {
        .receiver = Kind::Future,
        .storage = Storage::Static,
        .name = "wait",
        .operation = Operation::Wait,
        .kind = MemberKind::Method,
        .parameters = {"values", "eagerError", "cleanUp"},
        .required = 1,
    },
    {
        .receiver = Kind::Future,
        .storage = Storage::Static,
        .name = "race",
        .operation = Operation::Race,
        .kind = MemberKind::Method,
        .parameters = {"values", {}, {}},
        .required = 1,
    },
    {
        .receiver = Kind::Timer,
        .storage = Storage::Static,
        .name = "after",
        .operation = Operation::TimerAfter,
        .kind = MemberKind::Method,
        .parameters = {"milliseconds", "callback", {}},
        .required = 2,
    },
    {
        .receiver = Kind::Timer,
        .storage = Storage::Static,
        .name = "periodic",
        .operation = Operation::TimerPeriodic,
        .kind = MemberKind::Method,
        .parameters = {"milliseconds", "callback", {}},
        .required = 2,
    },
    {
        .receiver = Kind::Future,
        .storage = Storage::Instance,
        .name = "then",
        .operation = Operation::Then,
        .kind = MemberKind::Method,
        .parameters = {"onValue", "onError", {}},
        .required = 1,
    },
    {
        .receiver = Kind::Future,
        .storage = Storage::Instance,
        .name = "catchError",
        .operation = Operation::CatchError,
        .kind = MemberKind::Method,
        .parameters = {"onError", {}, {}},
        .required = 1,
    },
    {
        .receiver = Kind::Future,
        .storage = Storage::Instance,
        .name = "whenComplete",
        .operation = Operation::WhenComplete,
        .kind = MemberKind::Method,
        .parameters = {"action", {}, {}},
        .required = 1,
    },
    {
        .receiver = Kind::Future,
        .storage = Storage::Instance,
        .name = "timeout",
        .operation = Operation::Timeout,
        .kind = MemberKind::Method,
        .parameters = {"milliseconds", "onTimeout", {}},
        .required = 1,
    },
    {
        .receiver = Kind::Completer,
        .storage = Storage::Instance,
        .name = "future",
        .operation = Operation::CompleterFuture,
        .kind = MemberKind::Property,
        .parameters = {},
        .required = 0,
    },
    {
        .receiver = Kind::Completer,
        .storage = Storage::Instance,
        .name = "isCompleted",
        .operation = Operation::CompleterIsCompleted,
        .kind = MemberKind::Property,
        .parameters = {},
        .required = 0,
    },
    {
        .receiver = Kind::Completer,
        .storage = Storage::Instance,
        .name = "complete",
        .operation = Operation::CompleterComplete,
        .kind = MemberKind::Method,
        .parameters = {"value", {}, {}},
        .required = 1,
    },
    {
        .receiver = Kind::Completer,
        .storage = Storage::Instance,
        .name = "completeError",
        .operation = Operation::CompleterError,
        .kind = MemberKind::Method,
        .parameters = {"error", {}, {}},
        .required = 1,
    },
    {
        .receiver = Kind::Timer,
        .storage = Storage::Instance,
        .name = "cancel",
        .operation = Operation::TimerCancel,
        .kind = MemberKind::Method,
        .parameters = {},
        .required = 0,
    },
    {
        .receiver = Kind::CancellationToken,
        .storage = Storage::Instance,
        .name = "cancel",
        .operation = Operation::CancellationCancel,
        .kind = MemberKind::Method,
        .parameters = {},
        .required = 0,
    },
    {
        .receiver = Kind::CancellationToken,
        .storage = Storage::Instance,
        .name = "isCancelled",
        .operation = Operation::CancellationIsCancelled,
        .kind = MemberKind::Property,
        .parameters = {},
        .required = 0,
    },
});

const TypeDescriptor kVoid{.kind = Kind::Void, .name = kVoidTypeName};
const TypeDescriptor kInt{.kind = Kind::Int, .name = kIntTypeName};
const TypeDescriptor kBool{.kind = Kind::Bool, .name = kBoolTypeName};
const TypeDescriptor kError{.kind = Kind::Class, .name = kErrorTypeName};

TypeDescriptor FutureType(const TypeDescriptor& element) {
  return {
      .kind = Kind::Future,
      .name = InternTypeName(std::string(kFutureTypeName) + '<' + std::string(element.name) + '>'),
      .is_abstract = element.is_abstract,
  };
}

TypeDescriptor CallbackType(const TypeDescriptor& result,
                            std::span<const TypeDescriptor> parameters) {
  FunctionDescriptor signature;
  signature.return_type = result;
  for (const auto& parameter : parameters) {
    signature.AddParameter(std::to_string(signature.parameters.size()), parameter);
  }
  return MakeFunctionType(signature);
}

}  // namespace

std::span<const AsyncMember> AsyncMembers() noexcept {
  return kMembers;
}

const AsyncMember* FindAsyncMember(Kind receiver,
                                   MemberStorage storage,
                                   std::string_view name) noexcept {
  const auto* const found = std::ranges::find_if(kMembers, [&](const auto& member) {
    return member.receiver == receiver && member.storage == storage && member.name == name;
  });
  return found == kMembers.end() ? nullptr : &*found;
}

FunctionDescriptor MakeAsyncSignature(const AsyncMember& member,
                                      const TypeDescriptor& result,
                                      std::span<const TypeDescriptor> parameters) {
  FunctionDescriptor signature;
  signature.return_type = result;
  signature.storage = member.storage;
  signature.is_getter = member.kind == MemberKind::Property;
  using Presence = FunctionDescriptor::ParameterPresence;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    signature.AddParameter(std::string(member.parameters.at(index)), parameters[index],
                           index < member.required ? Presence::Required : Presence::Optional);
  }
  return signature;
}

namespace {

FunctionDescriptor DescribeFutureFactory(const AsyncMember& member, const TypeDescriptor& element) {
  auto result = FutureType(element);
  std::vector<TypeDescriptor> parameters;
  if (member.name == "value" || member.name == "error") {
    parameters = {member.name == "value" ? element : kError};
  } else if (member.name == "wait" || member.name == "race") {
    parameters = {MakeArrayType(result)};
    if (member.name == "wait") {
      result = FutureType(element.kind == Kind::Void ? element : MakeArrayType(element));
      const std::array input{element};
      parameters.push_back(kBool);
      parameters.push_back(MakeNullableType(
          CallbackType(kVoid, element.kind == Kind::Void ? std::span<const TypeDescriptor>()
                                                         : std::span(input))));
    }
  } else {
    if (member.name == "delayed") {
      parameters.push_back(kInt);
    }
    parameters.push_back(CallbackType(element, {}));
  }
  return MakeAsyncSignature(member, result, parameters);
}

FunctionDescriptor DescribeFutureMethod(const AsyncMember& member, const TypeDescriptor& element) {
  const TypeDescriptor output{.kind = Kind::Parameter, .name = "R", .is_abstract = true};
  const std::array input{element};
  const std::array error{kError};
  auto result = FutureType(element);
  std::vector<TypeDescriptor> parameters;
  switch (member.operation) {
    case Operation::Then:
      result = FutureType(output);
      parameters = {
          CallbackType(output, element.kind == Kind::Void ? std::span<const TypeDescriptor>()
                                                          : std::span(input)),
          MakeNullableType(CallbackType(output, error)),
      };
      break;
    case Operation::CatchError:
      parameters = {CallbackType(element, error)};
      break;
    case Operation::WhenComplete:
      parameters = {CallbackType(kVoid, {})};
      break;
    case Operation::Timeout:
      parameters = {kInt, MakeNullableType(CallbackType(element, {}))};
      break;
    default:
      std::unreachable();
  }
  return MakeAsyncSignature(member, result, parameters);
}

}  // namespace

FunctionDescriptor DescribeAsyncMember(const AsyncMember& member, const TypeDescriptor& element) {
  if (member.receiver == Kind::Invalid) {
    return MakeAsyncSignature(member, FutureType(kVoid), std::array{kInt});
  }
  if (member.storage == Storage::Static) {
    if (member.receiver == Kind::Timer) {
      return MakeAsyncSignature(member, {.kind = Kind::Timer, .name = kTimerTypeName},
                                std::array{kInt, CallbackType(kVoid, {})});
    }
    return DescribeFutureFactory(member, element);
  }
  if (member.receiver == Kind::Future) {
    return DescribeFutureMethod(member, element);
  }
  switch (member.operation) {
    case Operation::CompleterFuture:
      return MakeAsyncSignature(member, FutureType(element), {});
    case Operation::CompleterIsCompleted:
    case Operation::CancellationIsCancelled:
      return MakeAsyncSignature(member, kBool, {});
    case Operation::CompleterError:
      return MakeAsyncSignature(member, kVoid, std::array{kError});
    case Operation::CompleterComplete:
      if (element.kind != Kind::Void) {
        return MakeAsyncSignature(member, kVoid, std::array{element});
      }
      return MakeAsyncSignature(member, kVoid, {});
    case Operation::TimerCancel:
    case Operation::CancellationCancel:
      return MakeAsyncSignature(member, kVoid, {});
    default:
      std::unreachable();
  }
}

}  // namespace vela::frontend
