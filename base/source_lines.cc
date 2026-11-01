#include "base/source_lines.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace vela {

SourceLines IndexSourceLines(std::string_view text) {
  auto lines = std::make_shared<std::vector<std::size_t>>(1, 0);
  for (std::size_t offset = 0; offset < text.size(); ++offset) {
    if (text[offset] == '\n') {
      lines->push_back(offset + 1);
    }
  }
  return lines;
}

}  // namespace vela
