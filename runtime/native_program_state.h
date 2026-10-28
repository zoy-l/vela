#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "base/callable.h"
#include "runtime/member_table.h"
#include "runtime/native_function.h"
#include "runtime/native_image.h"
#include "runtime/object.h"
#include "runtime/program_layouts.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;

// 元数据由对应的 GC 程序状态对象拥有；其 managed 槽仅通过对象图追踪。
// 外部实例句柄及执行中的借用句柄负责保根，闭包和续延保留状态对象的引用。
struct NativeProgramState final {
  enum class GlobalState : std::uint8_t { Uninitialized, Initializing, Ready };
  static constexpr std::size_t kConstants = 0;
  static constexpr std::size_t kGlobals = 1;
  static constexpr std::size_t kStateSlots = 2;

  Isolate* isolate = nullptr;
  const NativeImage* image = nullptr;
  ProgramLayouts layouts;
  MemberTable members;
  const CodeIdentity* code_identity = nullptr;
  std::vector<CallSignature> signatures;
  std::vector<std::shared_ptr<const NativeFunction>> imports;
  std::vector<GlobalState> global_states;
  std::array<Value, kStateSlots> values;
};

}  // namespace vela::runtime
