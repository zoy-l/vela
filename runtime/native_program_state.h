#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "base/callable.h"
#include "base/declaration.h"
#include "runtime/member_table.h"
#include "runtime/native_function.h"
#include "runtime/native_image.h"
#include "runtime/object.h"
#include "runtime/program_layouts.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;
class NativeDispatch;
class NativeFunctionRoot;

// 缓存属于 Isolate 内的具体函数版本；共享机器码不会跨线程写同一缓存。
struct NativeFieldCache final {
  const NominalType* type = nullptr;
  std::uint64_t revision = 0;
  std::uint32_t slot = 0;
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

  NativeProgramState() = default;
  ~NativeProgramState();
  NativeProgramState(const NativeProgramState&) = delete;
  NativeProgramState& operator=(const NativeProgramState&) = delete;
  NativeProgramState(NativeProgramState&&) = delete;
  NativeProgramState& operator=(NativeProgramState&&) = delete;
  [[nodiscard]] NativeFieldCache& field_cache(std::uint64_t access);

  Isolate* isolate = nullptr;
  NativeDispatch* dispatch = nullptr;
  const NativeImage* image = nullptr;
  ProgramLayouts layouts;
  MemberTable members;
  const CodeIdentity* code_identity = nullptr;
  std::vector<CallSignature> signatures;
  std::vector<std::shared_ptr<const NativeFunction>> imports;
  NativeFunctionRoot* function_roots = nullptr;
  std::vector<std::vector<NativeFieldCache>> field_caches;
  // 数量由不可变 image 确定；独占定长存储供生成代码直接读取，不依赖 vector 布局。
  GlobalState* global_states = nullptr;
  std::array<Value, kStateSlots> values;
  // 动态映像使用别名 shared_ptr 保活其代码、重定位及借用元数据；静态 AOT 映像无需所有者。
  std::shared_ptr<const NativeImage> image_owner;
  std::vector<std::unique_ptr<ClassLayout>> owned_classes;
  std::vector<std::unique_ptr<NominalType>> owned_nominals;
  std::vector<std::unique_ptr<CodeIdentity>> owned_code_identities;
  // 清扫阶段先销毁对象载荷，再统一销毁版本元数据，避免死亡对象仍借用扫描描述。
  NativeProgramState* retired_next = nullptr;
};

}  // namespace vela::runtime
