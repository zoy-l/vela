#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include <vela/error.h>
#include <vela/handles.h>

#include "runtime/native/host_type.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_type.h"
#include "runtime/value/value.h"

namespace vela::runtime {

// 命名入口跟随当前发布；显式 Capture 才持有 NativeVersionEntry 的具体版本根。
class HostEntry final {
 public:
  [[nodiscard]] static Result<HostEntry> Bind(NativeInstance& instance,
                                              std::string_view name,
                                              NativeType signature);
  [[nodiscard]] static Result<HostEntry> Bind(NativeDispatch& dispatch,
                                              std::string_view module,
                                              std::string_view name,
                                              NativeType signature);
  [[nodiscard]] Result<HostEntry> Capture() const;
  [[nodiscard]] Result<void> Invoke(HandleScope& scope,
                                    std::span<const Value> arguments,
                                    Value& output) const;
  [[nodiscard]] Isolate& isolate() const noexcept { return signature_.isolate(); }

 private:
  using Target = std::variant<NativeNamedEntry, NativeVersionEntry>;
  HostEntry(Target target, HostType signature);
  [[nodiscard]] Result<NativeResolvedDeclaration> Resolve() const;
  [[nodiscard]] Result<void> Check(NativeResolvedDeclaration target) const;
  Target target_;
  HostType signature_;
  mutable std::optional<std::uint64_t> checked_revision_;
};

}  // namespace vela::runtime
