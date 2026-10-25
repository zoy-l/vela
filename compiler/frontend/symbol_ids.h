#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <absl/container/flat_hash_map.h>

namespace vela::frontend {

// 编译事务工作副本中的符号身份；新条目只追加，已有条目不重新编号。
class SymbolIds final {
 public:
  [[nodiscard]] std::uint32_t Get(const std::string& name) {
    return ids_.try_emplace(name, static_cast<std::uint32_t>(ids_.size())).first->second;
  }
  [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }

 private:
  absl::flat_hash_map<std::string, std::uint32_t> ids_;
};

}  // namespace vela::frontend
