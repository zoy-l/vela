#include "runtime/library/standard_library.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <memory>
#include <ratio>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "runtime/isolate/isolate.h"
#include "runtime/library/random.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_function.h"
#include "vela/error.h"

namespace vela::runtime {
namespace {

std::unexpected<vela::Error> Failure(std::string message) {
  return std::unexpected(vela::Error{.message = std::move(message), .stack_trace = {}});
}

double PerformanceNow() noexcept {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::int64_t DateNow() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void Seed(NativeContext& context, std::int64_t seed) noexcept {
  context.isolate().random().Seed(static_cast<std::uint64_t>(seed));
}

double NextDouble(NativeContext& context) noexcept {
  return context.isolate().random().NextDouble();
}

std::expected<std::int64_t, vela::Error> NextInt(NativeContext& context,
                                                 std::int64_t bound) noexcept {
  if (bound <= 0) {
    return Failure("random bound must be positive");
  }
  return context.isolate().random().NextInt(bound);
}

template <auto Operation>
double Unary(double value) noexcept {
  return Operation(value);
}

template <auto Operation>
double Binary(double left, double right) noexcept {
  return Operation(left, right);
}

bool IsFinite(double value) noexcept {
  return std::isfinite(value);
}

bool IsNaN(double value) noexcept {
  return std::isnan(value);
}

std::expected<double, vela::Error> Clamp(double value, double lower, double upper) noexcept {
  if (std::isnan(lower) || std::isnan(upper) || lower > upper) {
    return Failure("clamp bounds must be ordered and not NaN");
  }
  return std::fmin(std::fmax(value, lower), upper);
}

std::expected<double, vela::Error> Sqrt(double value) noexcept {
  if (value < 0.0) {
    return Failure("sqrt argument must be nonnegative");
  }
  return std::sqrt(value);
}

std::expected<double, vela::Error> Log(double value) noexcept {
  if (value <= 0.0) {
    return Failure("log argument must be positive");
  }
  return std::log(value);
}

}  // namespace

std::shared_ptr<const NativeFunction> FindStandardNative(std::string_view name) {
  static const auto standard_bindings = [] {
    absl::flat_hash_map<std::string, std::shared_ptr<const NativeFunction>> bindings;
    const auto add = [&bindings](NativeFunction function,
                                 NativeFunction::Intrinsic intrinsic =
                                     NativeFunction::Intrinsic::None) -> void {
      function.aot_entry_.intrinsic = intrinsic;
      const std::string binding_name(function.name());
      bindings.emplace(binding_name, std::make_shared<const NativeFunction>(std::move(function)));
    };
    add(NativeFunction::Bind<PerformanceNow>("performance.now"));
    add(NativeFunction::Bind<DateNow>("Date.now"));
    add(NativeFunction::Bind<Seed>("Random.seed"));
    add(NativeFunction::Bind<NextDouble>("Math.random"));
    add(NativeFunction::Bind<NextInt>("Random.nextInt"));
    add(NativeFunction::Bind<Sqrt>("Math.sqrt"), NativeFunction::Intrinsic::SquareRoot);
    add(NativeFunction::Bind<Log>("Math.log"));
    add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::sin)>>("Math.sin"));
    add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::cos)>>("Math.cos"));
    add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::tan)>>("Math.tan"));
    add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::floor)>>("Math.floor"));
    add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::ceil)>>("Math.ceil"));
    add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::round)>>("Math.round"));
    add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::trunc)>>("Math.trunc"));
    add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::fabs)>>("Math.abs"));
    add(NativeFunction::Bind<Unary<static_cast<double (*)(double)>(std::exp)>>("Math.exp"));
    add(NativeFunction::Bind<Binary<static_cast<double (*)(double, double)>(std::pow)>>(
        "Math.pow"));
    add(NativeFunction::Bind<Binary<static_cast<double (*)(double, double)>(std::fmin)>>(
        "Math.min"));
    add(NativeFunction::Bind<Binary<static_cast<double (*)(double, double)>(std::fmax)>>(
        "Math.max"));
    add(NativeFunction::Bind<Clamp>("Math.clamp"));
    add(NativeFunction::Bind<IsFinite>("Math.isFinite"));
    add(NativeFunction::Bind<IsNaN>("Math.isNaN"));
    return bindings;
  }();
  const auto found = standard_bindings.find(name);
  return found == standard_bindings.end() ? nullptr : found->second;
}

}  // namespace vela::runtime
