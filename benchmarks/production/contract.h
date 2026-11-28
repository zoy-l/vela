#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vela::benchmarks::production {

inline constexpr std::int64_t kSuccess = 42;
inline constexpr std::size_t kRingCapacity = 64;
inline constexpr std::int64_t kGenerationPeriod = 257;
inline constexpr std::uint64_t kDefaultProfileSamples = 10000;

enum class Kind : std::uint8_t {
  Filter,
  Find,
  FindMissing,
  Includes,
  IncludesMissing,
  NarrowFilter,
  IncludesObjects,
  IncludesStructs,
  IncludesIntegers,
  IncludesDoubles,
  IncludesNaN,
  Map,
  MapDuplicates,
  MapDynamic,
  MapNested,
  MapNaN,
  JsonScalar,
  JsonObject,
  JsonArray,
  JsonNested,
  JsonConstruct,
  AsyncNoAwait,
  AsyncCompleted,
  AsyncSuspend,
  AsyncChain,
  AsyncConcurrent,
  AsyncTimers,
  Churn,
};

enum class Family : std::uint8_t { Containers, Json, Async, Churn };

struct JsonContract final {
  std::string_view input;
  std::string_view code;
  std::string_view path;
  std::string_view expected;
  std::string_view actual;
  std::int64_t offset = -1;
};

enum class Measurement : std::uint8_t { Throughput, Latency, Allocations };

struct Options final {
  Measurement measurement = Measurement::Throughput;
  std::uint64_t samples = kDefaultProfileSamples;
};

struct Case final {
  std::string_view name;
  Kind kind;
  std::int64_t size;
  std::int64_t expected;
  std::uint32_t prepare;
  std::uint32_t function;
  std::uint32_t cleanup;
  Family family;
  JsonContract json;
};

}  // namespace vela::benchmarks::production
