#pragma once

#include <cstdint>
#include <string>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

// 参数绑定与名称访问经过 checker 时记录，不再第二遍遍历完整 AST。
class ParameterUsage final {
 public:
  void Declare(const Parameter& parameter, std::uint32_t binding);
  void Read(const Expression& expression, std::uint32_t binding);
  void Write(const Expression& expression, std::uint32_t binding);
  [[nodiscard]] Diagnostics DiagnosticsFor(const Program& program) const;

 private:
  struct ParameterReads final {
    std::string name;
    SourceLocation location;
    absl::flat_hash_set<const Expression*> reads;
  };
  absl::flat_hash_map<std::uint32_t, ParameterReads> parameters_;
};

}  // namespace vela::frontend
