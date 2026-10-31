#include "benchmarks/temporary_directory.h"

#include <filesystem>
#include <system_error>
#include <utility>

#include "benchmark/benchmark.h"

#include "base/testing/temporary_directory.h"

namespace vela::benchmarks {

BenchmarkDirectory::BenchmarkDirectory(benchmark::State& state) : state_(state) {
  auto created = ::vela::test::CreateTemporaryDirectory();
  if (!created) {
    state_.SkipWithError(created.error());
    return;
  }
  path_ = std::move(*created);
}

BenchmarkDirectory::~BenchmarkDirectory() {
  if (!IsValid()) {
    return;
  }
  std::error_code error;
  std::filesystem::remove_all(path_, error);
  if (error) {
    state_.SkipWithError(error.message());
  }
}

}  // namespace vela::benchmarks
