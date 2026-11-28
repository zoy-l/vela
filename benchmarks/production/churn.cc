#include "benchmarks/production/churn.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "benchmarks/production/allocation.h"
#include "benchmarks/production/contract.h"
#include "benchmarks/production/measurement.h"

namespace vela::benchmarks::production {

template <bool Counted>
class ChurnReference final {
 public:
  explicit ChurnReference(const Case& problem) : width_(static_cast<std::size_t>(problem.size)) {}
  std::int64_t Run() {
    const auto generation = sequence_ % kGenerationPeriod;
    Record record{generation, Text(generation), {}};
    for (std::size_t index = 0; index < width_; ++index) {
      record.values.push_back(generation + static_cast<std::int64_t>(index));
    }
    const auto slot = static_cast<std::size_t>(sequence_) % kCapacity;
    if (ring_.size() < kCapacity) {
      ring_.push_back(std::move(record));
    } else {
      ring_[slot] = std::move(record);
    }
    ++sequence_;
    for (std::size_t slot_index = 0; slot_index < ring_.size(); ++slot_index) {
      const auto& item = ring_[slot_index];
      const auto last = sequence_ - 1;
      const auto expected = last - ((last - static_cast<std::int64_t>(slot_index)) %
                                    static_cast<std::int64_t>(kCapacity));
      if (item.generation != expected % kGenerationPeriod) {
        return 0;
      }
      if (item.text != Text(item.generation) || item.values.size() != width_) {
        return 0;
      }
      for (std::size_t index = 0; index < width_; ++index) {
        if (item.values[index] != item.generation + static_cast<std::int64_t>(index)) {
          return 0;
        }
      }
    }
    return kSuccess;
  }
  void Cleanup() {
    ring_ = {};
    ring_.shrink_to_fit();
  }

 private:
  using String = std::basic_string<char, std::char_traits<char>, Allocator<char, Counted>>;
  struct Record final {
    std::int64_t generation;
    String text;
    std::vector<std::int64_t, Allocator<std::int64_t, Counted>> values;
  };
  static String Text(std::int64_t generation) {
    constexpr std::size_t kTextBuffer = 32;
    std::array<char, kTextBuffer> buffer{};
    const auto converted = std::to_chars(buffer.data(), std::to_address(buffer.end()), generation);
    String text("production-record-");
    text.append(buffer.data(), converted.ptr);
    return text;
  }
  static constexpr std::size_t kCapacity = kRingCapacity;
  std::size_t width_;
  std::int64_t sequence_ = 0;
  std::vector<Record, Allocator<Record, Counted>> ring_;
};

void RunChurn(benchmark::State& state, const Case& problem, const Options& options) {
  if (options.measurement == Measurement::Allocations) {
    AllocationLedger ledger;
    const AllocationScope scope(ledger);
    ChurnReference<true> reference(problem);
    RunReference<Measurement::Allocations>(state, problem, reference, options);
  } else {
    ChurnReference<false> reference(problem);
    if (options.measurement == Measurement::Latency) {
      RunReference<Measurement::Latency>(state, problem, reference, options);
    } else {
      RunReference<Measurement::Throughput>(state, problem, reference, options);
    }
  }
}

}  // namespace vela::benchmarks::production
