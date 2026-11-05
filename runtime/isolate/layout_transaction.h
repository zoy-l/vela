#pragma once

#include <cstdint>

#include "runtime/isolate/isolate.h"
#include "runtime/value/object.h"

namespace vela::runtime {

enum class LayoutLifetime : std::uint8_t { Isolate, Collectible };

// 只覆盖编译/加载阶段尚未产生实例的新增布局；Commit 后才允许产物与实例逃逸。
class LayoutTransaction final {
 public:
  explicit LayoutTransaction(Isolate& isolate,
                             LayoutLifetime lifetime = LayoutLifetime::Isolate) noexcept;
  ~LayoutTransaction();
  LayoutTransaction(const LayoutTransaction&) = delete;
  LayoutTransaction& operator=(const LayoutTransaction&) = delete;
  LayoutTransaction(LayoutTransaction&&) = delete;
  LayoutTransaction& operator=(LayoutTransaction&&) = delete;
  void Commit() noexcept { committed_ = true; }
  void CommitNative(HeapObject& owner);
  // 封闭映像共享缓存及存活值保留布局；不把共享身份转交某一个程序实例。
  void CommitImage();
  // 宿主 Type 的结构元数据由类型根追踪，不因绑定而升级为 Isolate 永久缓存。
  void CommitStructural();

 private:
  void RetainStructuralMetadata();
  Isolate* isolate_;
  LayoutCounts initial_;
  LayoutLifetime lifetime_;
  bool committed_ = false;
};

}  // namespace vela::runtime
