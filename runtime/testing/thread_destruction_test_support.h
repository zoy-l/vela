#pragma once

#include <thread>

namespace vela::runtime::test {

// 输出引用必须覆盖此标记的共享生命周期；测试在 join 或 owner 调度边界后读取。
class DestructionThread final {
 public:
  explicit DestructionThread(std::thread::id& output) : output_(&output) {}
  ~DestructionThread() { *output_ = std::this_thread::get_id(); }
  DestructionThread(const DestructionThread&) = delete;
  DestructionThread& operator=(const DestructionThread&) = delete;
  DestructionThread(DestructionThread&&) = delete;
  DestructionThread& operator=(DestructionThread&&) = delete;

 private:
  std::thread::id* output_;
};

}  // namespace vela::runtime::test
