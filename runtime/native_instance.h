#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

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
};

struct NativeInstantiationError final {
  NativeInstantiationStatus status = NativeInstantiationStatus::AbiMismatch;
  std::string message;
};

// 实例拥有本次实例化的绑定、常量根与全局状态；借用静态链接的不可变映像。
// Isolate 必须覆盖实例及其执行的生命周期，实例只能在所属线程使用和销毁。
class NativeInstance final {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<NativeInstance>, NativeInstantiationError>
  Instantiate(Isolate& isolate, const NativeImage& image);
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
  NativeInstance(NativeProgramState& data, Value anchor);
  [[nodiscard]] std::expected<void, NativeInstantiationError> BindImports(
      const layout::Program& description);
  [[nodiscard]] std::expected<void, NativeInstantiationError> InitializeConstants();

  NativeProgramState* data_;
  Value anchor_;
  RootRange anchor_root_;
};

}  // namespace vela::runtime
