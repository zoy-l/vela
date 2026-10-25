#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/debug_info.h"
#include "vm/exception_handler.h"

namespace vela::vm {

// 原生常量指针池地址稳定且持续保根；所属 Isolate 须覆盖程序和入口句柄的生命周期。
struct StringConstants final {
  StringConstants(runtime::Heap& heap, std::size_t count) : values(count), roots(heap, values) {}
  StringConstants(runtime::Heap& heap, std::span<runtime::StringObject* const> source)
      : values(source.begin(), source.end()), roots(heap, values) {}
  std::vector<runtime::StringObject*> values;
  runtime::RootRange roots;
};

// 全程序布局只保存一份；构造后只允许借用，不复制到每个函数。
class LayoutTables final {
 public:
  LayoutTables(std::vector<runtime::ClassLayout*> classes,
               std::vector<const runtime::ArrayLayout*> arrays,
               std::vector<const runtime::CollectionLayout*> collections,
               std::vector<runtime::TypeConstraint> types);
  LayoutTables(const LayoutTables&) = delete;
  auto operator=(const LayoutTables&) -> LayoutTables& = delete;
  LayoutTables(LayoutTables&&) = delete;
  auto operator=(LayoutTables&&) -> LayoutTables& = delete;
  ~LayoutTables() = default;

  [[nodiscard]] auto classes() const noexcept -> std::span<runtime::ClassLayout* const> {
    return classes_;
  }
  [[nodiscard]] auto arrays() const noexcept -> std::span<const runtime::ArrayLayout* const> {
    return arrays_;
  }
  [[nodiscard]] auto collections() const noexcept
      -> std::span<const runtime::CollectionLayout* const> {
    return collections_;
  }
  [[nodiscard]] auto types() const noexcept -> std::span<const runtime::TypeConstraint> {
    return types_;
  }

 private:
  std::vector<runtime::ClassLayout*> classes_;
  std::vector<const runtime::ArrayLayout*> arrays_;
  std::vector<const runtime::CollectionLayout*> collections_;
  std::vector<runtime::TypeConstraint> types_;
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
  std::shared_ptr<const std::vector<std::string>> source_paths;
  std::vector<SourceMapping> source_mappings;
  std::shared_ptr<const runtime::NativeFunction> native;
  std::vector<ExceptionHandler> handlers;
  std::vector<std::int64_t> integers;

  [[nodiscard]] auto View() const noexcept -> Function;
};

struct ValidationError final {
  std::size_t function = 0;
  std::size_t instruction = 0;
  std::string message;
};

enum class GlobalState : std::uint8_t { Uninitialized, Initializing, Ready };

// 每个编译程序拥有一份惰性静态状态；槽位地址固定并随程序持续保根。
struct GlobalStorage final {
  GlobalStorage(runtime::Heap& heap, std::vector<std::uint32_t> functions)
      : values(functions.size()),
        states(functions.size(), GlobalState::Uninitialized),
        initializers(std::move(functions)),
        roots(heap, values.data(), values.size()) {}
  std::vector<runtime::Value> values;
  std::vector<GlobalState> states;
  std::vector<std::uint32_t> initializers;
  runtime::RootRange roots;
};

struct ProgramStorage final {
  std::vector<FunctionStorage> functions;
  const runtime::Isolate* isolate = nullptr;
  std::shared_ptr<GlobalStorage> globals;
};

class FunctionHandle;

class Program final {
 public:
  Program() = default;

  [[nodiscard]] static auto Create(std::vector<FunctionStorage> functions,
                                   runtime::Isolate& isolate,
                                   std::vector<std::uint32_t> global_initializers = {})
      -> std::expected<Program, ValidationError>;
  [[nodiscard]] static auto Load(std::span<const Function> functions,
                                 runtime::Isolate& isolate,
                                 std::vector<std::uint32_t> global_initializers = {})
      -> std::expected<Program, ValidationError>;

  [[nodiscard]] auto functions() const noexcept -> std::span<const FunctionStorage>;
  [[nodiscard]] auto FindFunction(std::string_view name) const noexcept -> FunctionHandle;
  [[nodiscard]] auto IsForIsolate(const runtime::Isolate& isolate) const noexcept -> bool;
  [[nodiscard]] auto globals() const noexcept -> GlobalStorage*;

 private:
  friend class FunctionHandle;
  explicit Program(std::shared_ptr<const ProgramStorage> storage) : storage_(std::move(storage)) {}
  std::shared_ptr<const ProgramStorage> storage_;
};

class FunctionHandle final {
 public:
  FunctionHandle() = default;
  [[nodiscard]] auto IsValid() const noexcept -> bool;
  [[nodiscard]] auto program() const noexcept -> const Program& { return program_; }
  [[nodiscard]] auto index() const noexcept -> std::uint32_t { return index_; }
  [[nodiscard]] auto parameter_count() const noexcept -> std::uint16_t;
  [[nodiscard]] auto register_count() const noexcept -> std::uint16_t;
  [[nodiscard]] auto code() const noexcept -> std::span<const Instruction>;
  [[nodiscard]] auto doubles() const noexcept -> std::span<const double>;

 private:
  friend class Program;
  FunctionHandle(Program program, std::uint32_t index)
      : program_(std::move(program)), index_(index) {}
  Program program_;
  std::uint32_t index_ = 0;
};

}  // namespace vela::vm
