#include "benchmarks/production/containers.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/hash/hash.h>
#include <benchmark/benchmark.h>

#include "benchmarks/production/allocation.h"
#include "benchmarks/production/contract.h"
#include "benchmarks/production/measurement.h"

namespace vela::benchmarks::production {

constexpr std::int64_t kSeed = kSuccess;
constexpr std::size_t kLargeLiteral = 128;
constexpr std::size_t kSmallLiteral = 4;
constexpr std::size_t kDuplicateKeys = 8;
constexpr std::size_t kNaNEntries = 5;

struct Pair final {
  std::int64_t first;
  std::int64_t second;
  bool operator==(const Pair&) const = default;
};

struct SameDouble final {
  bool operator()(double left, double right) const noexcept {
    return left == right || (std::isnan(left) && std::isnan(right));
  }
};

struct DoubleHash final {
  std::size_t operator()(double value) const noexcept {
    auto canonical = value == 0.0 ? 0.0 : value;
    if (std::isnan(value)) {
      canonical = std::numeric_limits<double>::quiet_NaN();
    }
    return absl::HashOf(std::bit_cast<std::uint64_t>(canonical));
  }
};

template <class Key,
          class Value,
          bool Counted,
          class Hash = absl::Hash<Key>,
          class Equal = std::equal_to<Key>>
class OrderedMap final {
 public:
  explicit OrderedMap(std::size_t count) {
    if (count != 0) {
      entries_.reserve(count);
      positions_.reserve(count);
    }
  }
  void Set(Key key, Value value) {
    const auto [found, inserted] = positions_.try_emplace(key, entries_.size());
    if (inserted) {
      entries_.emplace_back(std::move(key), std::move(value));
    } else {
      entries_[found->second].second = std::move(value);
    }
  }
  [[nodiscard]] const auto& entries() const noexcept { return entries_; }

 private:
  std::vector<std::pair<Key, Value>, Allocator<std::pair<Key, Value>, Counted>> entries_;
  absl::flat_hash_map<Key,
                      std::size_t,
                      Hash,
                      Equal,
                      Allocator<std::pair<const Key, std::size_t>, Counted>>
      positions_;
};

template <class Map, class Project>
static std::int64_t MapChecksum(const Map& map, Project project) {
  const auto& entries = map.entries();
  auto total = static_cast<std::int64_t>(entries.size());
  std::int64_t rank = 0;
  for (const auto& [key, value] : entries) {
    total += ++rank * project(value);
  }
  return total;
}

template <Kind Kernel, bool Counted>
class ContainerReference final {
 public:
  explicit ContainerReference(const Case& problem)
      : count_(static_cast<std::size_t>(problem.size)) {
    if constexpr (Kernel == Kind::IncludesObjects) {
      objects_.resize(count_, Pair{.first = 1, .second = 0});
      pointers_.reserve(count_);
      for (const auto& item : objects_) {
        pointers_.push_back(&item);
      }
    } else if constexpr (Kernel == Kind::IncludesStructs) {
      objects_.resize(count_, Pair{.first = 1, .second = 2});
    } else if constexpr (Kernel == Kind::IncludesDoubles || Kernel == Kind::IncludesNaN) {
      tagged_.resize(count_, Tagged{1.0});
      if constexpr (Kernel == Kind::IncludesNaN) {
        tagged_.push_back(std::numeric_limits<double>::quiet_NaN());
        tagged_.push_back(-0.0);
      }
    } else if constexpr (Kernel == Kind::IncludesIntegers) {
      tagged_.resize(count_, Tagged{std::int64_t{1}});
    } else if constexpr (Kernel == Kind::MapNested) {
      keys_.reserve(count_);
      for (std::size_t index = 0; index < count_; ++index) {
        keys_.push_back("key" + std::to_string(index));
      }
    } else if constexpr (Kernel == Kind::Filter || Kernel == Kind::Find ||
                         Kernel == Kind::FindMissing || Kernel == Kind::Includes ||
                         Kernel == Kind::IncludesMissing) {
      integers_.resize(count_);
      std::iota(integers_.begin(), integers_.end(), kSeed);
    }
  }

  std::int64_t Run() {
    if constexpr (Kernel == Kind::Filter || Kernel == Kind::NarrowFilter) {
      return FilterValues();
    } else if constexpr (Kernel == Kind::Find || Kernel == Kind::Includes ||
                         Kernel == Kind::FindMissing || Kernel == Kind::IncludesMissing) {
      return SearchIntegers();
    } else if constexpr (Kernel == Kind::IncludesObjects || Kernel == Kind::IncludesStructs ||
                         Kernel == Kind::IncludesIntegers || Kernel == Kind::IncludesDoubles ||
                         Kernel == Kind::IncludesNaN) {
      return IncludesValues();
    } else {
      return ConstructMap();
    }
  }

 private:
  std::int64_t FilterValues() {
    Integers result;
    if constexpr (Kernel == Kind::Filter) {
      for (const auto value : integers_) {
        if (value % 2 == 0) {
          result.push_back(value);
        }
      }
    } else {
      const std::array<Mixed, 4> input{
          std::int64_t{1},
          std::string_view("text"),
          std::int64_t{kSeed},
          std::monostate{},
      };
      for (const auto& value : input) {
        if (const auto* number = std::get_if<std::int64_t>(&value);
            number != nullptr && *number > 0) {
          result.push_back(*number);
        }
      }
    }
    auto total = static_cast<std::int64_t>(result.size());
    for (std::size_t index = 0; index < result.size(); ++index) {
      total += static_cast<std::int64_t>(index + 1) * result[index];
    }
    return total;
  }

  std::int64_t SearchIntegers() {
    if constexpr (Kernel == Kind::FindMissing || Kernel == Kind::IncludesMissing) {
      return std::ranges::find(integers_, -1) == integers_.end() ? kSeed : 0;
    } else {
      constexpr std::size_t kQueries = 16;
      for (std::size_t offset = 1; offset <= kQueries; ++offset) {
        const auto target = kSeed + static_cast<std::int64_t>(integers_.size() - offset);
        const auto found = std::ranges::find(integers_, target);
        if (found == integers_.end() || *found != target) {
          return 0;
        }
      }
      return kSeed;
    }
  }

  std::int64_t IncludesValues() {
    if constexpr (Kernel == Kind::IncludesObjects) {
      const Pair needle{.first = kSeed, .second = 0};
      return std::ranges::find(pointers_, &needle) == pointers_.end() ? kSeed : 0;
    } else if constexpr (Kernel == Kind::IncludesStructs) {
      return std::ranges::find(objects_, Pair{.first = 1, .second = kSeed}) == objects_.end()
                 ? kSeed
                 : 0;
    } else if constexpr (Kernel == Kind::IncludesIntegers || Kernel == Kind::IncludesDoubles) {
      const Tagged needle =
          Kernel == Kind::IncludesIntegers ? Tagged{kSeed} : Tagged{static_cast<double>(kSeed)};
      return std::ranges::find(tagged_, needle) == tagged_.end() ? kSeed : 0;
    } else if constexpr (Kernel == Kind::IncludesNaN) {
      const auto includes = [&](double needle) {
        return std::ranges::any_of(tagged_, [&](const Tagged& value) {
          const auto* number = std::get_if<double>(&value);
          return number != nullptr && SameDouble{}(*number, needle);
        });
      };
      return includes(std::numeric_limits<double>::quiet_NaN()) && includes(0.0) &&
                     !includes(static_cast<double>(kSeed))
                 ? kSeed
                 : 0;
    }
  }

  std::int64_t ConstructMap() {
    if constexpr (Kernel == Kind::MapNaN) {
      OrderedMap<double, std::int64_t, Counted, DoubleHash, SameDouble> map(kNaNEntries);
      const auto nan = std::numeric_limits<double>::quiet_NaN();
      const std::array keys{nan, 0.0, -0.0, nan, 1.0};
      for (std::size_t index = 0; index < keys.size(); ++index) {
        map.Set(keys[index], static_cast<std::int64_t>(index + 1));
      }
      return MapChecksum(map, [](std::int64_t value) { return value; });
    } else if constexpr (Kernel == Kind::MapNested) {
      OrderedMap<std::string_view, Integers, Counted> map(count_);
      for (std::size_t index = 0; index < count_; ++index) {
        map.Set(keys_[index], Integers{static_cast<std::int64_t>(index + 1)});
      }
      return MapChecksum(map, [](const Integers& value) { return value[0]; });
    } else if constexpr (Kernel == Kind::MapDynamic) {
      OrderedMap<Tagged, Tagged, Counted> map(count_);
      for (std::size_t index = 0; index < count_; ++index) {
        const auto key = (index & 1U) == 0 ? Tagged{static_cast<std::int64_t>(index)}
                                           : Tagged{static_cast<double>(index)};
        map.Set(key, Tagged{static_cast<std::int64_t>(index + 1)});
      }
      return MapChecksum(map, [](const Tagged& value) { return std::get<std::int64_t>(value); });
    } else {
      if constexpr (Kernel == Kind::MapDuplicates) {
        return IntegerMap<kLargeLiteral>();
      }
      if (count_ == 0) {
        return IntegerMap<0>();
      }
      if (count_ == kSmallLiteral) {
        return IntegerMap<4>();
      }
      return IntegerMap<kLargeLiteral>();
    }
  }

  template <std::size_t Count>
  std::int64_t IntegerMap() {
    OrderedMap<std::int64_t, std::int64_t, Counted> map(Count);
    for (std::size_t index = 0; index < Count; ++index) {
      const auto key = Kernel == Kind::MapDuplicates ? index % kDuplicateKeys : index;
      map.Set(static_cast<std::int64_t>(key), static_cast<std::int64_t>(index + 1));
    }
    return MapChecksum(map, [](std::int64_t value) { return value; });
  }
  using Integers = std::vector<std::int64_t, Allocator<std::int64_t, Counted>>;
  using Tagged = std::variant<std::int64_t, double>;
  using Mixed = std::variant<std::monostate, std::int64_t, std::string_view>;
  std::size_t count_;
  Integers integers_;
  std::vector<Pair, Allocator<Pair, Counted>> objects_;
  std::vector<const Pair*, Allocator<const Pair*, Counted>> pointers_;
  std::vector<Tagged, Allocator<Tagged, Counted>> tagged_;
  std::vector<std::string, Allocator<std::string, Counted>> keys_;
};

template <Kind Kernel>
static void MeasureContainer(benchmark::State& state, const Case& problem, const Options& options) {
  if (options.measurement == Measurement::Allocations) {
    AllocationLedger ledger;
    const AllocationScope scope(ledger);
    ContainerReference<Kernel, true> reference(problem);
    RunReference<Measurement::Allocations>(state, problem, reference, options);
  } else {
    ContainerReference<Kernel, false> reference(problem);
    if (options.measurement == Measurement::Latency) {
      RunReference<Measurement::Latency>(state, problem, reference, options);
    } else {
      RunReference<Measurement::Throughput>(state, problem, reference, options);
    }
  }
}

void RunContainers(benchmark::State& state, const Case& problem, const Options& options) {
  switch (problem.kind) {
    case Kind::Filter: {
      MeasureContainer<Kind::Filter>(state, problem, options);
      return;
    }
    case Kind::Find: {
      MeasureContainer<Kind::Find>(state, problem, options);
      return;
    }
    case Kind::FindMissing: {
      MeasureContainer<Kind::FindMissing>(state, problem, options);
      return;
    }
    case Kind::Includes: {
      MeasureContainer<Kind::Includes>(state, problem, options);
      return;
    }
    case Kind::IncludesMissing: {
      MeasureContainer<Kind::IncludesMissing>(state, problem, options);
      return;
    }
    case Kind::NarrowFilter: {
      MeasureContainer<Kind::NarrowFilter>(state, problem, options);
      return;
    }
    case Kind::IncludesObjects: {
      MeasureContainer<Kind::IncludesObjects>(state, problem, options);
      return;
    }
    case Kind::IncludesStructs: {
      MeasureContainer<Kind::IncludesStructs>(state, problem, options);
      return;
    }
    case Kind::IncludesIntegers: {
      MeasureContainer<Kind::IncludesIntegers>(state, problem, options);
      return;
    }
    case Kind::IncludesDoubles: {
      MeasureContainer<Kind::IncludesDoubles>(state, problem, options);
      return;
    }
    case Kind::IncludesNaN: {
      MeasureContainer<Kind::IncludesNaN>(state, problem, options);
      return;
    }
    case Kind::Map: {
      MeasureContainer<Kind::Map>(state, problem, options);
      return;
    }
    case Kind::MapDuplicates: {
      MeasureContainer<Kind::MapDuplicates>(state, problem, options);
      return;
    }
    case Kind::MapDynamic: {
      MeasureContainer<Kind::MapDynamic>(state, problem, options);
      return;
    }
    case Kind::MapNested: {
      MeasureContainer<Kind::MapNested>(state, problem, options);
      return;
    }
    case Kind::MapNaN: {
      MeasureContainer<Kind::MapNaN>(state, problem, options);
      return;
    }
    default:
      state.SkipWithError("Non-container case sent to container reference");
  }
}

}  // namespace vela::benchmarks::production
