#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>
#include <benchmark/benchmark.h>

#include "runtime/value/value.h"

namespace vela::benchmarks {
namespace {

constexpr std::size_t kOperations = 128;
constexpr std::int64_t kExpectedSum = kOperations * (kOperations - 1) / 2;
constexpr std::string_view kModuleName = "main.vela";
constexpr std::string_view kTypeName = "Counter";
constexpr std::string_view kFieldName = "value";
constexpr std::string_view kMethodName = "add";

struct Counter final {
  std::int64_t value = 0;

  std::int64_t Add(std::int64_t argument) {
    value = argument;
    return value;
  }
};

enum class InputError : std::uint8_t { InvalidArgument };
enum class Operation : std::uint8_t { Field, Call };
enum class Strategy : std::uint8_t { Direct, Cached, MemberLookup, TypeMemberLookup };

runtime::Value Read(Counter& receiver) {
  return runtime::Value::Integer(receiver.value);
}

std::expected<void, InputError> Write(Counter& receiver, runtime::Value value) {
  if (!value.IsInteger()) {
    return std::unexpected(InputError::InvalidArgument);
  }
  receiver.value = value.AsInteger();
  return {};
}

std::expected<runtime::Value, InputError> Invoke(Counter& receiver,
                                                 std::span<const runtime::Value> arguments) {
  if (arguments.size() != 1 || !arguments.front().IsInteger()) {
    return std::unexpected(InputError::InvalidArgument);
  }
  return runtime::Value::Integer(receiver.Add(arguments.front().AsInteger()));
}

struct Field final {
  decltype(&Read) get;
  decltype(&Write) set;
};

struct Method final {
  decltype(&Invoke) invoke;
};

struct Type final {
  absl::flat_hash_map<std::string_view, Field> fields;
  absl::flat_hash_map<std::string_view, Method> methods;
};

// 这是固定 Counter& 接收者、单个 int 实参的手工登记参考，不具备 Vela 的完整反射契约。
// 查询失败、实参类型与数量的检查属于手工反射入口的成本，计时保留这些分支。
struct Registry final {
  absl::flat_hash_map<std::string_view, absl::flat_hash_map<std::string_view, Type>> modules{
      {
          kModuleName,
          {
              {
                  kTypeName,
                  {
                      .fields = {{kFieldName, {.get = Read, .set = Write}}},
                      .methods = {{kMethodName, {.invoke = Invoke}}},
                  },
              },
          },
      },
  };
};

std::expected<std::int64_t, InputError> Access(const Field& field,
                                               Counter& receiver,
                                               std::int64_t value) {
  if (const auto assigned = field.set(receiver, runtime::Value::Integer(value)); !assigned) {
    return std::unexpected(assigned.error());
  }
  const auto result = field.get(receiver);
  return result.AsInteger();
}

std::expected<std::int64_t, InputError> Access(const Method& method,
                                               Counter& receiver,
                                               std::int64_t value) {
  const std::array arguments{runtime::Value::Integer(value)};
  const auto result = method.invoke(receiver, arguments);
  if (!result) {
    return std::unexpected(result.error());
  }
  return result->AsInteger();
}

std::expected<const Type*, InputError> FindType(const Registry& registry) {
  const auto module = registry.modules.find(kModuleName);
  if (module == registry.modules.end()) {
    return std::unexpected(InputError::InvalidArgument);
  }
  const auto selected = module->second.find(kTypeName);
  if (selected == module->second.end()) {
    return std::unexpected(InputError::InvalidArgument);
  }
  return &selected->second;
}

template <Operation kOperation, Strategy kStrategy>
std::expected<std::int64_t, InputError> Perform(Counter& receiver,
                                                std::int64_t value,
                                                const Registry& registry,
                                                const Type& type,
                                                const Field& field,
                                                const Method& method) {
  if constexpr (kStrategy == Strategy::Direct) {
    if constexpr (kOperation == Operation::Field) {
      receiver.value = value;
      return receiver.value;
    } else {
      return receiver.Add(value);
    }
  }
  const Type* selected_type = &type;
  if constexpr (kStrategy == Strategy::TypeMemberLookup) {
    const auto selected = FindType(registry);
    if (!selected) {
      return std::unexpected(selected.error());
    }
    selected_type = *selected;
  }
  if constexpr (kOperation == Operation::Field) {
    const Field* selected = &field;
    if constexpr (kStrategy != Strategy::Cached) {
      const auto found = selected_type->fields.find(kFieldName);
      if (found == selected_type->fields.end()) {
        return std::unexpected(InputError::InvalidArgument);
      }
      selected = &found->second;
    }
    return Access(*selected, receiver, value);
  } else {
    const Method* selected = &method;
    if constexpr (kStrategy != Strategy::Cached) {
      const auto found = selected_type->methods.find(kMethodName);
      if (found == selected_type->methods.end()) {
        return std::unexpected(InputError::InvalidArgument);
      }
      selected = &found->second;
    }
    return Access(*selected, receiver, value);
  }
}

template <Operation kOperation, Strategy kStrategy>
std::expected<std::int64_t, InputError> Batch(const std::array<std::int64_t, kOperations>& values,
                                              Counter& receiver,
                                              const Registry& registry,
                                              const Type& type,
                                              const Field& field,
                                              const Method& method) {
  std::int64_t sum = 0;
  for (const auto value : values) {
    const auto result =
        Perform<kOperation, kStrategy>(receiver, value, registry, type, field, method);
    if (!result) {
      return std::unexpected(result.error());
    }
    sum += *result;
  }
  return sum;
}

template <Operation kOperation, Strategy kStrategy>
void CppWorkload(benchmark::State& state) {
  std::array<std::int64_t, kOperations> values{};
  for (std::size_t index = 0; index < values.size(); ++index) {
    values[index] = static_cast<std::int64_t>(index);
  }
  Counter receiver;
  Registry registry;
  const Type* type = &registry.modules.at(kModuleName).at(kTypeName);
  const Field* field = &type->fields.at(kFieldName);
  const Method* method = &type->methods.at(kMethodName);
  // 允许直接访问正常内联与向量化；缓存描述对象经优化屏障保留真实的间接调用。
  benchmark::DoNotOptimize(values.data());
  benchmark::DoNotOptimize(type);
  benchmark::DoNotOptimize(field);
  benchmark::DoNotOptimize(method);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    benchmark::ClobberMemory();
    auto result = Batch<kOperation, kStrategy>(values, receiver, registry, *type, *field, *method);
    if (!result || *result != kExpectedSum) {
      state.SkipWithError("C++ hand-registered reflection reference result is incorrect");
      return;
    }
    benchmark::DoNotOptimize(*result);
    benchmark::DoNotOptimize(receiver);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kOperations));
  state.counters["operations"] = kOperations;
  state.counters["result"] = kExpectedSum;
  state.counters["expected_result"] = kExpectedSum;
}

int RegisterCpp() {
  constexpr std::array kNames{"direct", "cached", "member_lookup", "type_member_lookup"};
  // 模板特化让直接访问保持可优化，不在每个操作中增加策略分支。
  constexpr std::array kFields{
      CppWorkload<Operation::Field, Strategy::Direct>,
      CppWorkload<Operation::Field, Strategy::Cached>,
      CppWorkload<Operation::Field, Strategy::MemberLookup>,
      CppWorkload<Operation::Field, Strategy::TypeMemberLookup>,
  };
  constexpr std::array kCalls{
      CppWorkload<Operation::Call, Strategy::Direct>,
      CppWorkload<Operation::Call, Strategy::Cached>,
      CppWorkload<Operation::Call, Strategy::MemberLookup>,
      CppWorkload<Operation::Call, Strategy::TypeMemberLookup>,
  };
  for (std::size_t index = 0; index < kNames.size(); ++index) {
    benchmark::RegisterBenchmark(std::string("reflection/cpp/field/") + kNames[index],
                                 kFields[index])
        ->UseRealTime();
    benchmark::RegisterBenchmark(std::string("reflection/cpp/call/") + kNames[index], kCalls[index])
        ->UseRealTime();
  }
  return 0;
}

[[maybe_unused]] const int kRegisteredCpp = RegisterCpp();

}  // namespace
}  // namespace vela::benchmarks
