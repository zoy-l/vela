#pragma once

#include <filesystem>

namespace benchmark {
class State;
}

namespace vela::benchmarks {

// 初始化和销毁失败都归属当前基准；提前返回同样释放已创建的目录。
class BenchmarkDirectory final {
 public:
  explicit BenchmarkDirectory(benchmark::State& state);
  ~BenchmarkDirectory();
  BenchmarkDirectory(const BenchmarkDirectory&) = delete;
  BenchmarkDirectory& operator=(const BenchmarkDirectory&) = delete;
  BenchmarkDirectory(BenchmarkDirectory&&) = delete;
  BenchmarkDirectory& operator=(BenchmarkDirectory&&) = delete;

  [[nodiscard]] bool IsValid() const noexcept { return !path_.empty(); }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  benchmark::State& state_;
  std::filesystem::path path_;
};

}  // namespace vela::benchmarks
