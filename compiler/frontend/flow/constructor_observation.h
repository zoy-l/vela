#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"

namespace vela::frontend {

// 父类构造期间按最终接收者分派后可能观察的字段；逃逸要求完整实例。
struct ConstructorObservation final {
  absl::flat_hash_set<std::uint32_t> fields;
  bool escapes_receiver = false;
};

[[nodiscard]] ConstructorObservation AnalyzeBaseConstruction(
    const ClassDescriptor& receiver,
    const ClassDescriptor& base,
    const absl::node_hash_map<std::string, ClassDescriptor>& classes,
    const absl::flat_hash_map<std::string_view, const Class*>& definitions);

}  // namespace vela::frontend
