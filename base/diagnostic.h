#pragma once

#include <iosfwd>
#include <string_view>

#include "base/source_location.h"

namespace vela {

void WriteError(std::ostream& output,
                std::string_view path,
                SourceLocation location,
                std::string_view message);

}  // namespace vela
