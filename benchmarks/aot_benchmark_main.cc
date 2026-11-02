#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "benchmarks/jit_benchmark.h"

namespace vela::benchmarks {
namespace {

// ReportContext 只在真正执行时调用，早于全部校准和采样；列出用例不会触发编译。
class JitPreparationReporter final : public benchmark::BenchmarkReporter {
 public:
  JitPreparationReporter() : reporter_(benchmark::CreateDefaultDisplayReporter()) {}

  bool ReportContext(const Context& context) override {
    const auto prepared = PrepareJitWorkloads(benchmark::GetBenchmarkFilter());
    if (!prepared) {
      std::cerr << "JIT benchmark preparation failed: " << prepared.error() << '\n';
      failed_ = true;
      return false;
    }
    benchmark::AddCustomContext("jit_prepared_suites", std::to_string(*prepared));
    return reporter_->ReportContext(context);
  }

  void ReportRunsConfig(double min_time,
                        bool has_explicit_iters,
                        benchmark::IterationCount iterations) override {
    reporter_->ReportRunsConfig(min_time, has_explicit_iters, iterations);
  }

  void ReportRuns(const std::vector<Run>& reports) override { reporter_->ReportRuns(reports); }
  void Finalize() override { reporter_->Finalize(); }
  [[nodiscard]] bool HasFailed() const noexcept { return failed_; }

 private:
  std::unique_ptr<benchmark::BenchmarkReporter> reporter_;
  bool failed_ = false;
};

}  // namespace
}  // namespace vela::benchmarks

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  vela::benchmarks::JitPreparationReporter reporter;
  benchmark::RunSpecifiedBenchmarks(&reporter);
  benchmark::Shutdown();
  return reporter.HasFailed() ? 1 : 0;
}
