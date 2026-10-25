#include "runtime/standard_library.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "base/utf8.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/random.h"

namespace vela::runtime {
namespace {

auto Failure(std::string message) -> std::unexpected<NativeError> {
  return std::unexpected(NativeError{.message = std::move(message)});
}

auto ReadLine(NativeContext& context) noexcept
    -> std::expected<std::optional<std::string>, NativeError> {
  auto& input = context.isolate().input();
  std::string line;
  if (!std::getline(input, line)) {
    if (input.eof() && !input.bad()) {
      return std::nullopt;
    }
    return Failure("console input failed");
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (InvalidUtf8Offset(line)) {
    return Failure("console input is not valid UTF-8");
  }
  return line;
}

auto Write(NativeContext& context, std::string_view text) noexcept
    -> std::expected<void, NativeError> {
  auto& output = context.isolate().output();
  output << text;
  if (!output) {
    return Failure("console output failed");
  }
  return {};
}

auto Flush(NativeContext& context) noexcept -> std::expected<void, NativeError> {
  auto& output = context.isolate().output();
  output.flush();
  if (!output) {
    return Failure("console output failed");
  }
  return {};
}

auto MonotonicMicros([[maybe_unused]] NativeContext& context) noexcept -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

auto UnixMicros([[maybe_unused]] NativeContext& context) noexcept -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void Seed(NativeContext& context, std::int64_t seed) noexcept {
  context.isolate().random().Seed(static_cast<std::uint64_t>(seed));
}

auto NextDouble(NativeContext& context) noexcept -> double {
  return context.isolate().random().NextDouble();
}

auto NextInt(NativeContext& context, std::int64_t bound) noexcept
    -> std::expected<std::int64_t, NativeError> {
  const auto value = context.isolate().random().NextInt(bound);
  if (!value) {
    return Failure("random bound must be positive");
  }
  return *value;
}

template <auto Operation>
auto Unary([[maybe_unused]] NativeContext& context, double value) noexcept -> double {
  return Operation(value);
}

template <auto Operation>
auto Binary([[maybe_unused]] NativeContext& context, double left, double right) noexcept -> double {
  return Operation(left, right);
}

auto IsFinite([[maybe_unused]] NativeContext& context, double value) noexcept -> bool {
  return std::isfinite(value);
}

auto IsNaN([[maybe_unused]] NativeContext& context, double value) noexcept -> bool {
  return std::isnan(value);
}

auto Clamp([[maybe_unused]] NativeContext& context,
           double value,
           double lower,
           double upper) noexcept -> std::expected<double, NativeError> {
  if (std::isnan(lower) || std::isnan(upper) || lower > upper) {
    return Failure("clamp bounds must be ordered and not NaN");
  }
  return std::fmin(std::fmax(value, lower), upper);
}

auto Sqrt([[maybe_unused]] NativeContext& context, double value) noexcept
    -> std::expected<double, NativeError> {
  if (value < 0.0) {
    return Failure("sqrt argument must be nonnegative");
  }
  return std::sqrt(value);
}

auto Log([[maybe_unused]] NativeContext& context, double value) noexcept
    -> std::expected<double, NativeError> {
  if (value <= 0.0) {
    return Failure("log argument must be positive");
  }
  return std::log(value);
}

auto MakeBindings() -> absl::flat_hash_map<std::string, std::shared_ptr<const NativeFunction>> {
  absl::flat_hash_map<std::string, std::shared_ptr<const NativeFunction>> bindings;
  const auto add = [&bindings](NativeFunction function) -> void {
    const std::string name(function.name());
    bindings.emplace(name, std::make_shared<const NativeFunction>(std::move(function)));
  };
  add(NativeFunction::Bind<ReadLine>("std:console.readLine"));
  add(NativeFunction::Bind<Write>("std:console.write"));
  add(NativeFunction::Bind<Flush>("std:console.flush"));
  add(NativeFunction::Bind<MonotonicMicros>("std:time.monotonicMicros"));
  add(NativeFunction::Bind<UnixMicros>("std:time.unixMicros"));
  add(NativeFunction::Bind<Seed>("std:random.seed"));
  add(NativeFunction::Bind<NextDouble>("std:random.nextDouble"));
  add(NativeFunction::Bind<NextInt>("std:random.nextInt"));
  add(NativeFunction::Bind<Sqrt>("std:math.sqrt"));
  add(NativeFunction::Bind<Log>("std:math.log"));
  add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::sin)>>("std:math.sin"));
  add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::cos)>>("std:math.cos"));
  add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::tan)>>("std:math.tan"));
  add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::floor)>>("std:math.floor"));
  add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::ceil)>>("std:math.ceil"));
  add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::round)>>("std:math.round"));
  add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::trunc)>>("std:math.trunc"));
  add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::fabs)>>("std:math.abs"));
  add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::exp)>>("std:math.exp"));
  add(NativeFunction::Bind<Binary<static_cast<double (*)(double, double)>(std::pow)>>(
      "std:math.pow"));
  add(NativeFunction::Bind<Binary<static_cast<double (*)(double, double)>(std::fmin)>>(
      "std:math.min"));
  add(NativeFunction::Bind<Binary<static_cast<double (*)(double, double)>(std::fmax)>>(
      "std:math.max"));
  add(NativeFunction::Bind<Clamp>("std:math.clamp"));
  add(NativeFunction::Bind<IsFinite>("std:math.isFinite"));
  add(NativeFunction::Bind<IsNaN>("std:math.isNaN"));
  return bindings;
}

}  // namespace

auto FindStandardNative(std::string_view name) -> std::shared_ptr<const NativeFunction> {
  static const auto bindings = MakeBindings();
  const auto found = bindings.find(name);
  return found == bindings.end() ? nullptr : found->second;
}

}  // namespace vela::runtime
