#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "runtime/external_resources.h"
#include "runtime/heap.h"
#include "runtime/native_image.h"
#include "runtime/native_instance.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;
class NativeDispatch;
struct NativeDispatchSnapshot;

enum class NativePublicationStatus : std::uint8_t {
  Stale,
  Stopped,
  InvalidIdentity,
  InstantiationFailed,
  Busy,
  RequiresReload,
};

struct NativePublicationError final {
  NativePublicationStatus status = NativePublicationStatus::InvalidIdentity;
  std::string message;
  DeclarationId declaration;
};

// 候选始终保根但不发布；所有分派索引在 Prepare 阶段完成，Publish 只交换所有权。
// 候选借用所属 Isolate，未发布时登记为外部资源，必须在 Reload 或销毁 Isolate 前释放。
class NativePreparedPublication final {
 public:
  ~NativePreparedPublication();
  NativePreparedPublication(NativePreparedPublication&&) noexcept;
  NativePreparedPublication& operator=(NativePreparedPublication&&) noexcept;
  NativePreparedPublication(const NativePreparedPublication&) = delete;
  NativePreparedPublication& operator=(const NativePreparedPublication&) = delete;

 private:
  friend class NativeDispatch;
  NativePreparedPublication(const NativeDispatch& owner,
                            std::uint64_t base_revision,
                            std::unique_ptr<NativeDispatchSnapshot> snapshot);
  const NativeDispatch* owner_;
  std::uint64_t base_revision_;
  std::unique_ptr<NativeDispatchSnapshot> snapshot_;
};

// 显式版本句柄保留其程序状态及代码；Invoke 不重新查询当前声明。
// 与 RootedValue 相同，只能在所属线程使用和释放，Isolate 必须覆盖所有副本。
class NativeVersionEntry final {
 public:
  [[nodiscard]] FunctionIdentity identity() const noexcept;
  [[nodiscard]] std::expected<Value, ExecutionStatus> Invoke(
      std::span<const Value> arguments) const;

 private:
  friend class NativeNamedEntry;
  struct Storage final {
    Storage(Isolate& isolate, Value source, std::uint32_t target);
    Value state;
    std::uint32_t function;
    RootRange root;
  };
  explicit NativeVersionEntry(std::shared_ptr<const Storage> storage);
  std::shared_ptr<const Storage> storage_;
};

// 命名入口只保存会话及声明，不保留任何历史映像。每次进入都选择当前已发布版本。
class NativeNamedEntry final {
 public:
  [[nodiscard]] DeclarationId declaration() const noexcept;
  [[nodiscard]] std::expected<NativeVersionEntry, ExecutionStatus> Capture() const;
  [[nodiscard]] std::expected<Value, ExecutionStatus> Invoke(
      std::span<const Value> arguments) const;

 private:
  friend class NativeDispatch;
  struct Storage final {
    Storage(NativeDispatch& source, DeclarationId target);
    NativeDispatch* dispatch;
    DeclarationId declaration;
    ExternalResource resource;
  };
  explicit NativeNamedEntry(std::shared_ptr<const Storage> storage);
  std::shared_ptr<const Storage> storage_;
};

struct NativeResolvedDeclaration final {
  HeapObject* state = nullptr;
  std::uint32_t function = 0;
};

// Isolate 拥有唯一发布入口。程序状态借用它；发布快照的根不会与 GC 状态形成共享所有权环。
// 所有操作及代码退休发生在 Isolate 所属线程。这里只负责分派发布机制；结构验证由事务完成。
class NativeDispatch final {
 public:
  explicit NativeDispatch(Isolate& isolate) noexcept;
  ~NativeDispatch();
  NativeDispatch(const NativeDispatch&) = delete;
  NativeDispatch& operator=(const NativeDispatch&) = delete;
  NativeDispatch(NativeDispatch&&) = delete;
  NativeDispatch& operator=(NativeDispatch&&) = delete;
  [[nodiscard]] std::uint64_t revision() const noexcept;
  [[nodiscard]] Isolate& isolate() const noexcept { return *isolate_; }
  // 只借用当前快照；使用者在可能分配前建立 NativeInstance 借用根。
  [[nodiscard]] HeapObject& current_state() const noexcept;
  [[nodiscard]] std::expected<NativePreparedPublication, NativePublicationError> Prepare(
      std::uint64_t base_revision,
      std::shared_ptr<const NativeImage> image);
  [[nodiscard]] std::expected<void, NativePublicationStatus> Publish(
      NativePreparedPublication&& candidate) noexcept;
  [[nodiscard]] std::expected<NativeNamedEntry, ExecutionStatus> FindEntry(
      std::string_view module_uri,
      std::string_view name);
  // 返回的目标只借用当前快照；调用方必须在下一个可提交边界之前保根其 state。
  [[nodiscard]] std::expected<NativeResolvedDeclaration, ExecutionStatus> Resolve(
      DeclarationId declaration) const noexcept;
  [[nodiscard]] std::expected<NativeResolvedDeclaration, ExecutionStatus> ResolveInterface(
      const NominalType& receiver,
      DeclarationId selector) const noexcept;
  [[nodiscard]] std::expected<Value, ExecutionStatus> LoadGlobal(NativeExecution& execution,
                                                                 DeclarationId declaration);
  [[nodiscard]] std::expected<void, ExecutionStatus> StoreGlobal(DeclarationId declaration,
                                                                 Value value) noexcept;
  void Close() noexcept;

 private:
  Isolate* isolate_;
  bool closed_ = false;
  std::unique_ptr<NativeDispatchSnapshot> current_;
};

extern "C" std::uint8_t vela_native_v3_load_global(NativeExecution* execution,
                                                   std::uint64_t declaration,
                                                   NativeValue* result);
extern "C" std::uint8_t vela_native_v3_store_global(NativeExecution* execution,
                                                    std::uint64_t declaration,
                                                    const NativeValue* value) noexcept;

}  // namespace vela::runtime
