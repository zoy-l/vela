#include "compiler/frontend/type_name.h"

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "compiler/frontend/type_names.h"

namespace vela::frontend {
namespace {

std::string_view CanonicalTypeName(std::string_view name) noexcept {
  if (name == kNullTypeName)
    return kNullTypeName;
  if (name == kVoidTypeName)
    return kVoidTypeName;
  if (name == kDynamicTypeName)
    return kDynamicTypeName;
  if (name == kBoolTypeName)
    return kBoolTypeName;
  if (name == kIntTypeName)
    return kIntTypeName;
  if (name == kDoubleTypeName)
    return kDoubleTypeName;
  if (name == kStringTypeName)
    return kStringTypeName;
  if (name == kMutableStringTypeName)
    return kMutableStringTypeName;
  if (name == kByteArrayTypeName)
    return kByteArrayTypeName;
  if (name == kByteDataTypeName)
    return kByteDataTypeName;
  if (name == kRegExpTypeName)
    return kRegExpTypeName;
  if (name == kRegExpMatchTypeName)
    return kRegExpMatchTypeName;
  if (name == kEndianTypeName)
    return kEndianTypeName;
  if (name == kFutureTypeName)
    return kFutureTypeName;
  if (name == kCompleterTypeName)
    return kCompleterTypeName;
  if (name == kTimerTypeName)
    return kTimerTypeName;
  if (name == kCancellationTokenTypeName)
    return kCancellationTokenTypeName;
  if (name == kSetTypeName)
    return kSetTypeName;
  if (name == kMapTypeName)
    return kMapTypeName;
  if (name == kErrorTypeName)
    return kErrorTypeName;
  if (name == kTimeoutExceptionTypeName)
    return kTimeoutExceptionTypeName;
  if (name == kDateTypeName)
    return kDateTypeName;
  if (name == kDatePartsTypeName)
    return kDatePartsTypeName;
  if (name == kJsonTypeName)
    return kJsonTypeName;
  if (name == kMathTypeName)
    return kMathTypeName;
  if (name == kRandomTypeName)
    return kRandomTypeName;
  if (name == kPerformanceTypeName)
    return kPerformanceTypeName;
  if (name == kSleepFunctionName)
    return kSleepFunctionName;
  return {};
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
  if (name.empty())
    return {};
  if (const auto canonical = CanonicalTypeName(name); !canonical.empty())
    return canonical;
  auto& pool = Pool();
  const std::lock_guard lock(pool.mutex);
  if (const auto found = pool.values.find(name); found != pool.values.end()) {
    return *found;
  }
  return *pool.values.emplace(name).first;
}

std::string_view InternTypeName(std::string&& name) {
  if (name.empty())
    return {};
  if (const auto canonical = CanonicalTypeName(name); !canonical.empty())
    return canonical;
  auto& pool = Pool();
  const std::lock_guard lock(pool.mutex);
  if (const auto found = pool.values.find(name); found != pool.values.end()) {
    return *found;
  }
  return *pool.values.emplace(std::move(name)).first;
}

}  // namespace vela::frontend
