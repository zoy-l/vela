#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "runtime/external_resources.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/native_control.h"
#include "runtime/native_dispatch.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "runtime/program_layouts.h"
#include "runtime/random.h"

namespace vela::runtime {

struct NativeImage;
class NativeInstance;

// 会话内固定的宿主实现与签名；复制快照只共享不可变 NativeFunction 包装。
class NativeBindings final {
 public:
  NativeBindings() = default;

 private:
  friend class Isolate;
  absl::flat_hash_map<std::string, std::shared_ptr<const NativeFunction>> functions_;
};

struct LayoutCounts final {
  std::size_t classes = 0;
  std::size_t arrays = 0;
  std::size_t collections = 0;
  std::size_t signatures = 0;
  std::size_t interfaces = 0;
  std::size_t code_identities = 0;
  std::size_t nominal_types = 0;
  bool operator==(const LayoutCounts&) const = default;
};

class Isolate final {
 public:
  explicit Isolate(std::size_t gc_threshold = kDefaultGcThreshold) noexcept;
  explicit Isolate(NativeBindings bindings,
                   std::size_t gc_threshold = kDefaultGcThreshold) noexcept;
  ~Isolate();

  Isolate(Isolate&&) = delete;
  Isolate& operator=(Isolate&&) = delete;
  Isolate(const Isolate&) = delete;
  Isolate& operator=(const Isolate&) = delete;

  [[nodiscard]] Heap& heap() noexcept { return heap_; }
  [[nodiscard]] const Heap& heap() const noexcept { return heap_; }
  [[nodiscard]] ClassLayout& error_layout() noexcept { return *error_layout_; }
  [[nodiscard]] const ClassLayout& error_layout() const noexcept { return *error_layout_; }
  [[nodiscard]] ClassLayout& date_parts_layout() noexcept { return *date_parts_layout_; }
  [[nodiscard]] const ClassLayout& date_parts_layout() const noexcept {
    return *date_parts_layout_;
  }
  [[nodiscard]] Random& random() noexcept { return random_; }
  [[nodiscard]] FutureScheduler& scheduler() noexcept { return scheduler_; }
  [[nodiscard]] NativeDispatch& native_dispatch() noexcept { return native_dispatch_; }
  [[nodiscard]] NativeControl& native_control() noexcept { return native_control_; }
  [[nodiscard]] ExternalResources& external_resources() noexcept {
    return scheduler_.external_resources();
  }
  [[nodiscard]] std::vector<ExternalResourceInfo> ReloadBlockers() const;
  // 宿主先停止产生新借用并释放句柄；拒绝时不关闭端点、不丢弃排队任务。
  [[nodiscard]] std::expected<void, std::vector<ExternalResourceInfo>> TryStop();
  [[nodiscard]] std::ostream& output() noexcept;
  void SetOutput(std::ostream& output) noexcept;

  [[nodiscard]] ClassLayout* CreateClassLayout(std::uint32_t field_count,
                                               std::span<const std::uint32_t> reference_fields,
                                               ClassLayoutOptions options = {});
  [[nodiscard]] const ArrayLayout* CreateArrayLayout(ArrayLayout layout);
  [[nodiscard]] const CollectionLayout* CreateCollectionLayout(CollectionLayout layout);
  [[nodiscard]] const TypeSignature* CreateTypeSignature(TypeSignature signature);
  [[nodiscard]] const InterfaceType* CreateInterfaceType();
  [[nodiscard]] const CodeIdentity* CreateCodeIdentity();
  [[nodiscard]] NominalType* CreateNominalType(NominalTypeOptions options);
  [[nodiscard]] std::expected<void, NativeRegistrationError> RegisterNative(
      NativeFunction function);
  [[nodiscard]] std::shared_ptr<const NativeFunction> FindNative(std::string_view name) const;
  [[nodiscard]] NativeBindings FreezeBindings();
  [[nodiscard]] LayoutCounts layout_counts() const noexcept;

 private:
  friend class LayoutTransaction;
  friend class NativeInstance;
  std::vector<std::unique_ptr<ClassLayout>> class_layouts_;
  std::vector<std::unique_ptr<NominalType>> nominal_types_;
  std::vector<std::unique_ptr<ArrayLayout>> array_layouts_;
  std::vector<std::unique_ptr<CollectionLayout>> collection_layouts_;
  std::vector<std::unique_ptr<TypeSignature>> type_signatures_;
  std::vector<std::unique_ptr<InterfaceType>> interface_types_;
  std::vector<std::unique_ptr<CodeIdentity>> code_identities_;
  absl::flat_hash_map<ArrayLayout, const ArrayLayout*> array_layout_index_;
  absl::flat_hash_map<CollectionLayout, const CollectionLayout*> collection_layout_index_;
  absl::flat_hash_map<TypeSignature, const TypeSignature*> type_signature_index_;
  absl::flat_hash_map<std::string, std::shared_ptr<const NativeFunction>> natives_;
  bool bindings_frozen_ = false;
  struct NativeImageCache final {
    LayoutCache layouts;
    std::weak_ptr<const NativeImage> owner;
    bool static_lifetime = true;
  };
  absl::flat_hash_map<const NativeImage*, NativeImageCache> native_images_;
  ClassLayout* error_layout_ = nullptr;
  ClassLayout* date_parts_layout_ = nullptr;
  Random random_;
  FutureScheduler scheduler_;
  std::ostream* output_ = nullptr;
  Heap heap_;
  NativeControl native_control_;
  NativeDispatch native_dispatch_;
};

}  // namespace vela::runtime
