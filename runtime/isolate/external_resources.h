#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace vela::runtime {

enum class ExternalResourceKind : std::uint8_t {
  RootRange,
  RootedValue,
  NativeInstance,
  NativeHandle,
  Future,
  Execution,
  HostAccess,
  NativeEntry,
  NativeCandidate,
};

struct ExternalResourceInfo final {
  ExternalResourceKind kind = ExternalResourceKind::RootRange;
  std::uint64_t identity = 0;
  std::size_t references = 0;
};

[[nodiscard]] std::string_view ExternalResourceKindName(ExternalResourceKind kind) noexcept;

class ExternalResource;

// 登记与移除只在所属线程发生；句柄计数允许宿主线程复制和释放 Future。
// 登记本身不拥有资源，也不保活脚本对象，不能替代 GC 根追踪。
class ExternalResources final {
 public:
  ExternalResources() = default;
  ExternalResources(const ExternalResources&) = delete;
  ExternalResources& operator=(const ExternalResources&) = delete;
  ExternalResources(ExternalResources&&) = delete;
  ExternalResources& operator=(ExternalResources&&) = delete;
  ~ExternalResources() = default;
  void AppendTo(std::vector<ExternalResourceInfo>& result) const;

 private:
  friend class ExternalResource;
  ExternalResource* first_ = nullptr;
  std::uint64_t next_identity_ = 0;
};

// 登记覆盖共享存储的生命周期；只有外部句柄或当前执行作用域增加 references。
class ExternalResource final {
 public:
  ExternalResource() = default;
  ExternalResource(ExternalResources& owner, ExternalResourceKind kind) noexcept;
  ~ExternalResource();
  ExternalResource(const ExternalResource&) = delete;
  ExternalResource& operator=(const ExternalResource&) = delete;
  ExternalResource(ExternalResource&&) = delete;
  ExternalResource& operator=(ExternalResource&&) = delete;
  void Attach(ExternalResources& owner, ExternalResourceKind kind) noexcept;
  void Retain() noexcept;
  void Release() noexcept;
  [[nodiscard]] std::size_t references() const noexcept;

 private:
  friend class ExternalResources;
  ExternalResource* next_ = nullptr;
  ExternalResource** previous_link_ = nullptr;
  ExternalResourceKind kind_ = ExternalResourceKind::RootRange;
  std::uint64_t identity_ = 0;
  std::atomic_size_t references_{0};
};

}  // namespace vela::runtime
