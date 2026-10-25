#include "base/diagnostic.h"

#include <ostream>
#include <string_view>
#include "base/source_location.h"

namespace vela {

void WriteError(std::ostream& output,
                std::string_view path,
                SourceLocation location,
                std::string_view message) {
  if (path.empty()) {
    output << "vela";
  } else {
    output << path << ':' << location.line << ':' << location.column;
  }
  output << ": error: " << message << '\n';
}

}  // namespace vela
