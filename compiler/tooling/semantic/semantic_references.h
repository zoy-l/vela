#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "compiler/tooling/semantic/semantic_index.h"

namespace vela::tooling {

enum class ReferenceDeclaration : std::uint8_t { Exclude, Include };

// 保留源码声明身份、使用位置与方法契约关系，不持有各入口重复的 AST 和检查模型。
class ReferenceIndex final {
 public:
  void AddOccurrence(const Definition& declaration, Definition location, bool is_declaration);
  void Connect(const Definition& first, const Definition& second);
  [[nodiscard]] std::vector<Definition> FindReferences(const Definition& declaration,
                                                       ReferenceDeclaration include) const;

 private:
  using Key = std::tuple<std::string, std::size_t, std::size_t>;
  struct Occurrence final {
    Definition location;
    bool is_declaration = false;
  };
  [[nodiscard]] static Key KeyFor(const Definition& location);
  absl::flat_hash_map<Key, std::vector<Occurrence>> references_;
  absl::flat_hash_map<Key, absl::flat_hash_set<Key>> relations_;
};

}  // namespace vela::tooling
