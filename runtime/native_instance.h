#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "runtime/heap.h"
#include "runtime/member_table.h"
#include "runtime/native_abi.h"
#include "runtime/native_function.h"
#include "runtime/native_image.h"
#include "runtime/native_program_state.h"
#include "runtime/object.h"
#include "runtime/program_layouts.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;

enum class NativeInstantiationStatus : std::uint8_t {
  AbiMismatch,
  TargetMismatch,
  MissingImport,
  ImportSignatureMismatch,
  OutOfMemory,
  MissingSession,
};

struct NativeInstantiationError final {
  NativeInstantiationStatus status = NativeInstantiationStatus::AbiMismatch;
  std::string message;
};

// 发布快照持有会话的声明到名义身份映射；具体布局版本从不写入此表。
struct NativeTypeIdentities final {
  absl::flat_hash_map<DeclarationId, NominalType*> classes;
  absl::flat_hash_map<DeclarationId, const InterfaceType*> interfaces;
};

// 实例拥有本次实例化的绑定、常量根与全局状态。动态映像的共享所有者随 GC 程序状态保活，
// 覆盖闭包、执行与续延；静态链接映像的借用字节须覆盖这些引用的生命周期。
// Isolate 必须覆盖实例及其执行的生命周期，实例只能在所属线程使用和销毁。
class NativeInstance final {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<NativeInstance>, NativeInstantiationError>
  Instantiate(Isolate& isolate, const NativeImage& image);
  [[nodiscard]] static std::expected<std::unique_ptr<NativeInstance>, NativeInstantiationError>
  Instantiate(Isolate& isolate, std::shared_ptr<const NativeImage> image);
  // 借用对象图中的程序状态；只在当前执行作用域内保根，不建立永久任务根。
  [[nodiscard]] static NativeInstance Borrow(HeapObject& state);
  ~NativeInstance() = default;
  NativeInstance(const NativeInstance&) = delete;
  NativeInstance& operator=(const NativeInstance&) = delete;
  NativeInstance(NativeInstance&&) = delete;
  NativeInstance& operator=(NativeInstance&&) = delete;

  [[nodiscard]] Isolate& isolate() const noexcept { return *data_->isolate; }
  [[nodiscard]] const NativeImage& image() const noexcept { return *data_->image; }
  [[nodiscard]] const ProgramLayouts& layouts() const noexcept { return data_->layouts; }
  [[nodiscard]] const MemberTable& members() const noexcept { return data_->members; }
  [[nodiscard]] const CodeIdentity& code_identity() const noexcept { return *data_->code_identity; }
  [[nodiscard]] const CallSignature& call_signature(std::size_t function) const noexcept {
    return data_->signatures.at(function);
  }
  [[nodiscard]] const NativeFunction::AotEntry& import_entry(std::size_t function) const noexcept {
    return data_->imports.at(function)->aot_entry();
  }
  [[nodiscard]] std::span<const Value> state() const noexcept { return {&anchor_, 1}; }
  [[nodiscard]] Value constant(std::size_t index) const noexcept;
  [[nodiscard]] std::expected<Value, ExecutionStatus> InitializeGlobal(NativeExecution& execution,
                                                                       std::size_t index);
  void StoreGlobal(std::size_t index, Value value) noexcept;

 private:
  friend struct NativeMemoryLayout;
  friend class NativeDispatch;
  NativeInstance(NativeProgramState& data, Value anchor, RootOwner root_owner);
  [[nodiscard]] static std::expected<std::unique_ptr<NativeInstance>, NativeInstantiationError>
  Instantiate(Isolate& isolate,
              const NativeImage& image,
              std::shared_ptr<const NativeImage> owner,
              NativeTypeIdentities* identities);
  [[nodiscard]] std::expected<void, NativeInstantiationError> BindImports(
      const layout::Program& description);
  [[nodiscard]] std::expected<void, NativeInstantiationError> InitializeConstants();

  NativeProgramState* data_;
  Value anchor_;
  RootRange anchor_root_;
};

}  // namespace vela::runtime
