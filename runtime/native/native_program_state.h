#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include "base/layout_description.h"

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "runtime/isolate/member_table.h"
#include "runtime/isolate/program_layouts.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_image.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Isolate;
class NativeDispatch;
class NativeFunctionRoot;
struct NativeGlobalCache;
class ReflectionIndex;

// 缓存属于 Isolate 内的具体函数版本；共享机器码不会跨线程写同一缓存。
struct NativeFieldCache final {
  std::uint64_t layout_id = 0;
  std::uint32_t slot = 0;
  std::uint64_t kind_offset = 0;
  [[nodiscard]] std::optional<std::uint32_t> Resolve(const ClassLayout& layout,
                                                     DeclarationId field) noexcept;
};

// 元数据由对应的 GC 程序状态对象拥有；其 managed 槽仅通过对象图追踪。
// 外部实例句柄及执行中的借用句柄负责保根，闭包和续延保留状态对象的引用。
struct NativeProgramState final {
  enum class GlobalState : std::uint8_t { Uninitialized, Initializing, Ready };
  static constexpr std::size_t kConstants = 0;
  static constexpr std::size_t kGlobals = 1;
  static constexpr std::size_t kStateSlots = 2;

  NativeProgramState();
  ~NativeProgramState();
  NativeProgramState(const NativeProgramState&) = delete;
  NativeProgramState& operator=(const NativeProgramState&) = delete;
  NativeProgramState(NativeProgramState&&) = delete;
  NativeProgramState& operator=(NativeProgramState&&) = delete;
  [[nodiscard]] NativeFieldCache& field_cache(std::uint64_t access);
  // 映像模式已在实例化边界验证：closed 使用状态数组，reloadable 使用 epoch 缓存。
  [[nodiscard]] GlobalState& global_state(std::size_t index) const noexcept;
  [[nodiscard]] NativeGlobalCache& global_cache(std::size_t index) const noexcept;

  Isolate* isolate = nullptr;
  NativeDispatch* dispatch = nullptr;
  const NativeImage* image = nullptr;
  ProgramLayouts layouts;
  MemberTable members;
  std::unique_ptr<ReflectionIndex> reflection;
  absl::flat_hash_map<const NominalType*, std::uint32_t> class_indices;
  absl::flat_hash_map<std::pair<const NominalType*, std::string_view>, std::uint32_t> enum_members;
  absl::flat_hash_map<const layout::Program*, TypeConstraint> native_future_types;
  const CodeIdentity* code_identity = nullptr;
  std::vector<CallSignature> signatures;
  std::vector<std::shared_ptr<const NativeFunction>> imports;
  NativeFunctionRoot* function_roots = nullptr;
  std::vector<std::vector<NativeFieldCache>> field_caches;
  // 仅在稳定槽序与源描述完全一致时允许常量偏移访问；槽序演化的版本继续按声明解析。
  std::vector<const ClassLayout*> direct_field_layouts;
  // 数量由不可变 image 确定；Heap 核算并由本状态独占的定长存储供生成代码直接读取。
  GlobalState* global_states = nullptr;
  NativeGlobalCache* global_caches = nullptr;
  std::array<Value, kStateSlots> values;
  // 动态映像使用别名 shared_ptr 保活其代码、重定位及借用元数据；静态 AOT 映像无需所有者。
  std::shared_ptr<const NativeImage> image_owner;
  std::vector<std::unique_ptr<ClassLayout>> owned_classes;
  std::vector<std::unique_ptr<NominalType>> owned_nominals;
  std::vector<std::unique_ptr<CodeIdentity>> owned_code_identities;
  // 清扫阶段先销毁对象载荷，再统一销毁版本元数据，避免死亡对象仍借用扫描描述。
  NativeProgramState* retired_next = nullptr;
};

// 槽由当前分派快照保活；读写者先验证 epoch，才能解引用 state/value/owner。
// 发布或关闭分派先使代数失效，因此弱缓存不会保活已删除全局值或读取退休槽。
struct NativeGlobalCache final {
  const std::uint64_t* epoch = nullptr;
  std::uint64_t observed = 0;
  NativeProgramState::GlobalState* state = nullptr;
  Value* value = nullptr;
  HeapObject* owner = nullptr;
};

}  // namespace vela::runtime
