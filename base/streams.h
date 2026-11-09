#pragma once

#include <functional>
#include <iosfwd>

namespace vela {

struct Streams final {
  std::reference_wrapper<std::istream> input;
  std::reference_wrapper<std::ostream> output;
  std::reference_wrapper<std::ostream> errors;
};

}  // namespace vela
