#include "benchmarks/production/async.h"
#include "benchmarks/production/async_runtime.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

#include "benchmarks/production/allocation.h"
#include "benchmarks/production/contract.h"
#include "benchmarks/production/measurement.h"

namespace vela::benchmarks::production {

template <bool Counted>
static Task<Counted> Immediate([[maybe_unused]] Scheduler<Counted>* scheduler, std::int64_t value) {
  co_return value;
}

template <bool Counted>
static Task<Counted> Completed(Scheduler<Counted>* scheduler) {
  co_return co_await Immediate(scheduler, kSuccess);
}

template <bool Counted>
static Task<Counted> Suspended(Scheduler<Counted>* scheduler) {
  co_await typename Scheduler<Counted>::Yield{.scheduler = scheduler};
  co_return kSuccess;
}

template <bool Counted>
static Task<Counted> Then(Scheduler<Counted>* scheduler, Task<Counted>* previous) {
  const auto value = co_await *previous;
  co_await typename Scheduler<Counted>::Yield{.scheduler = scheduler};
  co_return value + 1;
}

template <bool Counted>
static Task<Counted> WaitEvent([[maybe_unused]] Scheduler<Counted>* scheduler,
                               Event<Counted>* event) {
  const std::vector<std::int64_t, Allocator<std::int64_t, Counted>> live{40, 2};
  co_await *event;
  co_return live[0] + live[1];
}

template <bool Counted>
static Task<Counted> SignalEvent(Scheduler<Counted>* scheduler, Event<Counted>* event) {
  co_await typename Scheduler<Counted>::Delay{.scheduler = scheduler, .duration = {}};
  event->Signal(scheduler);
  co_return 0;
}

template <bool Counted>
static Task<Counted> TimerTask(Scheduler<Counted>* scheduler, std::int64_t* total) {
  co_await typename Scheduler<Counted>::Delay{.scheduler = scheduler, .duration = {}};
  ++*total;
  co_return 0;
}

template <bool Counted>
static Task<Counted> TimerBarrier(Scheduler<Counted>* scheduler, const std::int64_t* total) {
  co_await typename Scheduler<Counted>::Delay{.scheduler = scheduler, .duration = {}};
  co_return *total;
}

template <bool Counted>
static Task<Counted> Gather([[maybe_unused]] Scheduler<Counted>* scheduler,
                            std::vector<Task<Counted>, Allocator<Task<Counted>, Counted>>* tasks) {
  std::vector<std::int64_t, Allocator<std::int64_t, Counted>> values;
  values.reserve(tasks->size());
  for (auto& task : *tasks) {
    values.push_back(co_await task);
  }
  std::int64_t total = 0;
  for (const auto value : values) {
    total += value;
  }
  co_return total;
}

template <bool Counted>
class AsyncReference final {
 public:
  explicit AsyncReference(const Case& problem) : problem_(problem) {}
  std::int64_t Run() {
    Scheduler<Counted> scheduler;
    if (problem_.kind == Kind::AsyncNoAwait) {
      return Single(scheduler, Immediate(&scheduler, kSuccess));
    }
    if (problem_.kind == Kind::AsyncCompleted) {
      return Single(scheduler, Completed(&scheduler));
    }
    if (problem_.kind == Kind::AsyncSuspend) {
      return Single(scheduler, Suspended(&scheduler));
    }
    std::vector<Task<Counted>, Allocator<Task<Counted>, Counted>> tasks;
    const auto count = static_cast<std::size_t>(problem_.size);
    tasks.reserve(count + 1);
    if (problem_.kind == Kind::AsyncChain) {
      tasks.push_back(Immediate(&scheduler, 0));
      for (std::size_t index = 0; index < count; ++index) {
        tasks.push_back(Then(&scheduler, &tasks.back()));
      }
      scheduler.Drain();
      return tasks.back().IsDone() ? tasks.back().value() : -1;
    }
    if (problem_.kind == Kind::AsyncConcurrent) {
      Event<Counted> event;
      for (std::size_t index = 0; index < count; ++index) {
        tasks.push_back(WaitEvent(&scheduler, &event));
      }
      const auto producer = SignalEvent(&scheduler, &event);
      const auto aggregate = Gather(&scheduler, &tasks);
      scheduler.Drain();
      return producer.IsDone() && aggregate.IsDone() ? aggregate.value() : -1;
    }
    std::int64_t total = 0;
    for (std::size_t index = 0; index < count; ++index) {
      tasks.push_back(TimerTask(&scheduler, &total));
    }
    return Single(scheduler, TimerBarrier(&scheduler, &total));
  }

 private:
  static std::int64_t Single(Scheduler<Counted>& scheduler, Task<Counted> task) {
    scheduler.Drain();
    return task.IsDone() ? task.value() : -1;
  }
  Case problem_;
};

void RunAsync(benchmark::State& state, const Case& problem, const Options& options) {
  if (options.measurement == Measurement::Allocations) {
    AllocationLedger ledger;
    const AllocationScope scope(ledger);
    AsyncReference<true> reference(problem);
    RunReference<Measurement::Allocations>(state, problem, reference, options);
  } else {
    AsyncReference<false> reference(problem);
    if (options.measurement == Measurement::Latency) {
      RunReference<Measurement::Latency>(state, problem, reference, options);
    } else {
      RunReference<Measurement::Throughput>(state, problem, reference, options);
    }
  }
}

}  // namespace vela::benchmarks::production
