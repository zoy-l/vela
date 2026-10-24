#include "runtime/isolate.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "runtime/object.h"
#include "runtime/string.h"

namespace vela::runtime {

Isolate::Isolate(std::size_t gc_threshold) noexcept : heap_(gc_threshold) {}

auto Isolate::CreateClassLayout(std::uint32_t field_count,
                                std::span<const std::uint32_t> reference_fields) -> ClassLayout* {
  std::vector<std::uint32_t> references(reference_fields.begin(), reference_fields.end());
  std::ranges::sort(references);
  const auto duplicates = std::ranges::unique(references);
  references.erase(duplicates.begin(), duplicates.end());
  std::erase_if(references,
                [field_count](std::uint32_t index) -> bool { return index >= field_count; });

  std::unique_ptr<ClassLayout> layout(new ClassLayout(field_count, std::move(references)));
  ClassLayout* result = layout.get();
  class_layouts_.push_back(std::move(layout));
  return result;
}

auto Isolate::CreateString(std::string value) -> StringObject* {
  std::unique_ptr<StringObject> string =
      std::make_unique<StringObject>(StringObject{std::move(value)});
  StringObject* result = string.get();
  strings_.push_back(std::move(string));
  return result;
}

auto Isolate::CreateArrayLayout(ArrayLayout layout) -> const ArrayLayout* {
  auto stored = std::make_unique<ArrayLayout>(layout);
  const auto* result = stored.get();
  array_layouts_.push_back(std::move(stored));
  return result;
}

}  // namespace vela::runtime
