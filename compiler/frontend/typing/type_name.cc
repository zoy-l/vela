#include "compiler/frontend/typing/type_name.h"

#include <absl/container/flat_hash_set.h>
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
  if (name == kNullTypeName) {
    return kNullTypeName;
  }
  if (name == kSleepFunctionName) {
    return kSleepFunctionName;
  }

  static const absl::flat_hash_set<std::string_view> kCanonical(kBuiltinTypeNames.begin(),
                                                                kBuiltinTypeNames.end());
  const auto found = kCanonical.find(name);
  return found == kCanonical.end() ? std::string_view() : *found;
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

template <class Name>
std::string_view Intern(Name&& name) {
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
  return *pool.values.emplace(std::forward<Name>(name)).first;
}

}  // namespace

std::string_view InternTypeName(std::string_view name) {
  return Intern(name);
}

std::string_view InternTypeName(std::string&& name) {
  return Intern(std::move(name));
}

}  // namespace vela::frontend
