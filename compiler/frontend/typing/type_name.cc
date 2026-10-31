#include "compiler/frontend/typing/type_name.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {
namespace {

std::string_view CanonicalTypeName(std::string_view name) noexcept {
  static constexpr auto names = std::to_array<std::string_view>({
      kNullTypeName,
      kVoidTypeName,
      kDynamicTypeName,
      kBoolTypeName,
      kIntTypeName,
      kDoubleTypeName,
      kStringTypeName,
      kMutableStringTypeName,
      kByteArrayTypeName,
      kByteDataTypeName,
      kRegExpTypeName,
      kRegExpMatchTypeName,
      kEndianTypeName,
      kFutureTypeName,
      kCompleterTypeName,
      kTimerTypeName,
      kCancellationTokenTypeName,
      kSetTypeName,
      kMapTypeName,
      kErrorTypeName,
      kTimeoutExceptionTypeName,
      kDateTypeName,
      kDatePartsTypeName,
      kJsonTypeName,
      kMathTypeName,
      kRandomTypeName,
      kPerformanceTypeName,
      kSleepFunctionName,
  });
  const auto found = std::ranges::find(names, name);
  return found == names.end() ? std::string_view() : *found;
}

struct TypeNameHash final {
  using is_transparent = void;

  std::size_t operator()(std::string_view name) const noexcept {
    return std::hash<std::string_view>{}(name);
  }

  std::size_t operator()(const std::string& name) const noexcept {
    return (*this)(std::string_view(name));
  }
};

struct TypeNameEqual final {
  using is_transparent = void;

  bool operator()(std::string_view left, std::string_view right) const noexcept {
    return left == right;
  }
};

struct TypeNamePool final {
  std::mutex mutex;
  std::unordered_set<std::string, TypeNameHash, TypeNameEqual> values;
};

TypeNamePool& Pool() {
  static TypeNamePool pool;
  return pool;
}

}  // namespace

std::string_view InternTypeName(std::string_view name) {
  if (name.empty()) {
    return {};
  }
  if (const auto canonical = CanonicalTypeName(name); !canonical.empty()) {
    return canonical;
  }
  auto& pool = Pool();
  const std::scoped_lock lock(pool.mutex);
  if (const auto found = pool.values.find(name); found != pool.values.end()) {
    return *found;
  }
  return *pool.values.emplace(name).first;
}

std::string_view InternTypeName(std::string&& name) {
  if (name.empty()) {
    return {};
  }
  if (const auto canonical = CanonicalTypeName(name); !canonical.empty()) {
    return canonical;
  }
  auto& pool = Pool();
  const std::scoped_lock lock(pool.mutex);
  if (const auto found = pool.values.find(name); found != pool.values.end()) {
    return *found;
  }
  return *pool.values.emplace(std::move(name)).first;
}

}  // namespace vela::frontend
