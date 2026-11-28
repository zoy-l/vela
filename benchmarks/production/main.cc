#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <benchmark/benchmark.h>

#include "benchmarks/production/aot.h"
#include "benchmarks/production/async.h"
#include "benchmarks/production/churn.h"
#include "benchmarks/production/containers.h"
#include "benchmarks/production/contract.h"
#include "benchmarks/production/json.h"
#include "benchmarks/production_native/entries.h"

namespace vela::benchmarks::production {

static std::expected<Options, std::string> ReadOptions(int& argc, char** argv) {
  Options options;
  int retained = 1;
  const std::span arguments(argv, static_cast<std::size_t>(argc) + 1);
  constexpr std::string_view kSamples = "--profile-samples=";
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(arguments[static_cast<std::size_t>(index)]);
    if (argument == "--profile=latency") {
      options.measurement = Measurement::Latency;
    } else if (argument == "--profile=allocations") {
      options.measurement = Measurement::Allocations;
    } else if (argument.starts_with("--profile=")) {
      return std::unexpected("profile must be latency or allocations");
    } else if (argument.starts_with(kSamples)) {
      const auto value = argument.substr(kSamples.size());
      const auto* end = std::to_address(value.end());
      const auto parsed = std::from_chars(std::to_address(value.begin()), end, options.samples);
      if (parsed.ec != std::errc{} || parsed.ptr != end || options.samples == 0 ||
          options.samples >
              static_cast<std::uint64_t>(std::numeric_limits<benchmark::IterationCount>::max())) {
        return std::unexpected("profile-samples must be a positive integer");
      }
    } else {
      arguments[static_cast<std::size_t>(retained++)] = arguments[static_cast<std::size_t>(index)];
    }
  }
  argc = retained;
  arguments[static_cast<std::size_t>(retained)] = nullptr;
  return options;
}

using Runner = void (*)(benchmark::State&, const Case&, const Options&);

static Runner CppRunner(Family family) {
  switch (family) {
    case Family::Containers:
      return RunContainers;
    case Family::Json:
      return RunJson;
    case Family::Async:
      return RunAsync;
    case Family::Churn:
      return RunChurn;
  }
  std::unreachable();
}

static std::string_view ModeName(Measurement mode) {
  switch (mode) {
    case Measurement::Throughput:
      return "throughput";
    case Measurement::Latency:
      return "latency";
    case Measurement::Allocations:
      return "allocations";
  }
  std::unreachable();
}

static void RegisterCases(const Options& options) {
  for (const auto& problem : kCases) {
    for (const auto backend : {std::string_view("cpp"), std::string_view("aot")}) {
      auto* registration = benchmark::RegisterBenchmark(
          "production/" + std::string(backend) + "/" + std::string(problem.name),
          backend == "aot" ? RunAot : CppRunner(problem.family), problem, options);
      if (options.measurement == Measurement::Latency) {
        registration->Iterations(static_cast<benchmark::IterationCount>(options.samples))
            ->UseManualTime();
      } else {
        registration->UseRealTime();
        if (options.measurement == Measurement::Allocations) {
          registration->Iterations(static_cast<benchmark::IterationCount>(options.samples));
        }
      }
    }
  }
}

}  // namespace vela::benchmarks::production

int main(int argc, char** argv) {
  const auto options = vela::benchmarks::production::ReadOptions(argc, argv);
  if (!options) {
    std::cerr << options.error() << '\n';
    return 2;
  }
  vela::benchmarks::production::RegisterCases(*options);
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 2;
  }
  benchmark::AddCustomContext("production_contract", "paired-aot-cpp/owned-results/v1");
  benchmark::AddCustomContext(
      "measurement_mode",
      std::string(vela::benchmarks::production::ModeName(options->measurement)));
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
