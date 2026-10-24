#pragma once

#include <iosfwd>

#include "runtime/value.h"

namespace vela::runtime {

void WriteValue(std::ostream& output, Value value);

}  // namespace vela::runtime
