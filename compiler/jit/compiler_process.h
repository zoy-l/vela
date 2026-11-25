#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace vela::compiler::jit {

enum class CompilerCommand : std::uint8_t;

using CompilerStream = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
struct CompilerStreams final {
  CompilerStream input{nullptr, &std::fclose};
  CompilerStream output{nullptr, &std::fclose};
};
#ifdef _WIN32
using CompilerProcessHandle = void*;
#else
using CompilerProcessHandle = std::uintptr_t;
#endif

// 通道拥有一个编译子进程；最后一个延迟函数版本释放后关闭管道并等待其退出。
class CompilerProcess final {
 public:
  static std::expected<std::shared_ptr<CompilerProcess>, std::string> Start(
      const std::filesystem::path& executable);
  CompilerProcess(CompilerStreams streams, CompilerProcessHandle process);
  CompilerProcess(const CompilerProcess&) = delete;
  CompilerProcess& operator=(const CompilerProcess&) = delete;
  CompilerProcess(CompilerProcess&&) = delete;
  CompilerProcess& operator=(CompilerProcess&&) = delete;
  ~CompilerProcess();
  std::expected<std::vector<std::byte>, std::string> Receive() const;
  void StartReceiving();
  // 回调只唤醒消费者，不重入 Exchange；注销等待当前通知结束。
  void SetFileChanged(std::uint64_t resource_id, std::function<void()> changed);
  void Release(CompilerCommand command, std::uint64_t resource_id);
  std::expected<std::vector<std::byte>, std::string> Exchange(std::span<const std::byte> request);

 private:
  void ReadMessages();
  void CloseStreams();
  CompilerStreams streams_;
  CompilerProcessHandle process_;
  std::mutex mutex_;
  std::mutex response_mutex_;
  std::condition_variable response_ready_;
  std::optional<std::vector<std::byte>> response_;
  std::optional<std::string> failure_;
  std::mutex callbacks_mutex_;
  absl::flat_hash_map<std::uint64_t, std::function<void()>> callbacks_;
  std::thread receiver_;
};

}  // namespace vela::compiler::jit
