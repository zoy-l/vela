#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/callable.h"
#include "base/execution_status.h"
#include "base/member.h"
#include "runtime/external_resources.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/member_table.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/debug_info.h"
#include "vm/exception_handler.h"

namespace vela::vm {

namespace internal {
class Execution;
}

// 原生常量指针池地址稳定且持续保根；所属 Isolate 须覆盖程序和入口句柄的生命周期。
struct StringConstants final {
  StringConstants(runtime::Heap& heap, std::size_t count)
      : values(count), roots(heap, values, runtime::RootOwner::Runtime) {}
  StringConstants(runtime::Heap& heap, std::span<runtime::StringObject* const> source)
      : values(source.begin(), source.end()), roots(heap, values, runtime::RootOwner::Runtime) {}
  std::vector<runtime::StringObject*> values;
  runtime::RootRange roots;
};

// 全程序布局只保存一份；构造后只允许借用，不复制到每个函数。
class LayoutTables final {
 public:
  LayoutTables(std::vector<runtime::ClassLayout*> classes,
               std::vector<const runtime::ArrayLayout*> arrays,
               std::vector<const runtime::CollectionLayout*> collections,
               std::vector<runtime::TypeConstraint> types,
               std::vector<MemberDefinition> members = {});
  LayoutTables(const LayoutTables&) = delete;
  LayoutTables& operator=(const LayoutTables&) = delete;
  LayoutTables(LayoutTables&&) = delete;
  LayoutTables& operator=(LayoutTables&&) = delete;
  ~LayoutTables() = default;

  [[nodiscard]] std::span<runtime::ClassLayout* const> classes() const noexcept { return classes_; }
  [[nodiscard]] std::span<const runtime::ArrayLayout* const> arrays() const noexcept {
    return arrays_;
  }
  [[nodiscard]] std::span<const runtime::CollectionLayout* const> collections() const noexcept {
    return collections_;
  }
  [[nodiscard]] std::span<const runtime::TypeConstraint> types() const noexcept { return types_; }
  [[nodiscard]] const std::vector<MemberDefinition>& members() const noexcept {
    return members_.members();
  }
  [[nodiscard]] const MemberDefinition* FindMember(runtime::Value receiver,
                                                   std::string_view name) const;
  [[nodiscard]] bool IsCanonicalReceiverType(std::uint32_t index) const;

 private:
  std::vector<runtime::ClassLayout*> classes_;
  std::vector<const runtime::ArrayLayout*> arrays_;
  std::vector<const runtime::CollectionLayout*> collections_;
  std::vector<runtime::TypeConstraint> types_;
  runtime::MemberTable members_;
};

// 可变构建输入只在创建程序时使用，成功后所有权移入不可变共享存储。
struct FunctionStorage final {
  std::string name;
  std::vector<Instruction> code;
  std::shared_ptr<const LayoutTables> layouts;
  std::shared_ptr<const StringConstants> strings;
  std::vector<double> doubles;
  std::uint16_t register_count = 0;
  std::uint16_t parameter_count = 0;
  bool is_async = false;
  std::shared_ptr<const std::vector<std::string>> source_paths;
  std::vector<SourceMapping> source_mappings;
  std::shared_ptr<const runtime::NativeFunction> native;
  std::vector<ExceptionHandler> handlers;
  std::vector<std::int64_t> integers;
  std::optional<CallSignature> signature = std::nullopt;

  [[nodiscard]] Function View() const noexcept;
};

struct ValidationError final {
  std::size_t function = 0;
  std::size_t instruction = 0;
  std::string message;
};

enum class GlobalState : std::uint8_t { Uninitialized, Initializing, Ready };

// REPL 各次编译共享惰性全局状态；外部 Program 句柄保根，续延通过值图追踪槽位。
struct GlobalStorage final {
  GlobalStorage(runtime::Heap& heap, std::vector<std::uint32_t> functions);
  [[nodiscard]] std::expected<void, ExecutionStatus> Extend(std::vector<std::uint32_t> functions);
  void Retain() noexcept;
  void Release() noexcept;
  void Store(std::size_t index, runtime::Value value) noexcept;
  runtime::Heap& owner;
  runtime::Value storage;
  std::span<runtime::Value> values;
  std::vector<GlobalState> states;
  std::vector<std::uint32_t> initializers;
  runtime::RootRange roots{runtime::RootOwner::Runtime};
  std::size_t rooted_users = 0;
};

struct ProgramStorage final {
  ProgramStorage(std::vector<FunctionStorage> code,
                 const runtime::CodeIdentity* identity,
                 runtime::Isolate& owner,
                 std::shared_ptr<GlobalStorage> state);
  std::vector<FunctionStorage> functions;
  const runtime::CodeIdentity* code_identity = nullptr;
  const runtime::Isolate* isolate = nullptr;
  std::shared_ptr<GlobalStorage> globals;
  mutable runtime::ExternalResource external;
};

class FunctionHandle;

class Program final {
 public:
  Program() = default;
  ~Program();
  Program(const Program& other);
  Program& operator=(const Program& other);
  Program(Program&& other) noexcept;
  Program& operator=(Program&& other) noexcept;

  [[nodiscard]] static std::expected<Program, ValidationError> Create(
      std::vector<FunctionStorage> functions,
      runtime::Isolate& isolate,
      std::vector<std::uint32_t> global_initializers = {},
      std::shared_ptr<GlobalStorage> globals = {},
      const runtime::CodeIdentity* code_identity = nullptr);
  [[nodiscard]] std::shared_ptr<GlobalStorage> shared_globals() const noexcept;
  [[nodiscard]] static std::expected<Program, ValidationError> Load(
      std::span<const Function> functions,
      runtime::Isolate& isolate,
      std::vector<std::uint32_t> global_initializers = {});

  [[nodiscard]] std::span<const FunctionStorage> functions() const noexcept;
  [[nodiscard]] FunctionHandle FindFunction(std::string_view name) const noexcept;
  [[nodiscard]] bool IsForIsolate(const runtime::Isolate& isolate) const noexcept;
  // 会话拥有的强根与宿主句柄分开；返回宿主入口时重新建立 External 引用。
  [[nodiscard]] Program Retained() const;
  [[nodiscard]] GlobalStorage* globals() const noexcept;
  [[nodiscard]] const runtime::CodeIdentity& code_identity() const noexcept {
    return *storage_->code_identity;
  }

 private:
  friend class FunctionHandle;
  friend class internal::Execution;
  enum class Rooting : std::uint8_t { External, Retained, Continuation };
  Program(std::shared_ptr<const ProgramStorage> storage, Rooting rooting = Rooting::External);
  [[nodiscard]] Program Internal() const;
  void Retain() noexcept;
  void Release() noexcept;
  std::shared_ptr<const ProgramStorage> storage_;
  Rooting rooting_ = Rooting::External;
};

class FunctionHandle final {
 public:
  FunctionHandle() = default;
  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] const Program& program() const noexcept { return program_; }
  [[nodiscard]] std::uint32_t index() const noexcept { return index_; }
  [[nodiscard]] std::uint16_t parameter_count() const noexcept;
  [[nodiscard]] bool is_async() const noexcept;
  [[nodiscard]] std::uint16_t register_count() const noexcept;
  [[nodiscard]] std::span<const Instruction> code() const noexcept;

 private:
  friend class Program;
  FunctionHandle(Program program, std::uint32_t index)
      : program_(std::move(program)), index_(index) {}
  Program program_;
  std::uint32_t index_ = 0;
};

}  // namespace vela::vm
