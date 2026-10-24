#include "runtime/isolate.h"

#include <algorithm>

namespace vela::runtime {

Isolate::Isolate(std::size_t gc_threshold) noexcept : heap_(gc_threshold) {}

ClassLayout* Isolate::CreateClassLayout(std::uint32_t field_count,
                                        std::span<const std::uint32_t> reference_fields) {
  std::vector<std::uint32_t> references(reference_fields.begin(), reference_fields.end());
  std::sort(references.begin(), references.end());
  references.erase(std::unique(references.begin(), references.end()), references.end());
  references.erase(
      std::remove_if(references.begin(), references.end(),
                     [field_count](std::uint32_t index) { return index >= field_count; }),
      references.end());

  std::unique_ptr<ClassLayout> layout(new ClassLayout(field_count, std::move(references)));
  ClassLayout* result = layout.get();
  class_layouts_.push_back(std::move(layout));
  return result;
}

}  // namespace vela::runtime
