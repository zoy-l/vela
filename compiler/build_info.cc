#include "compiler/build_info.h"

namespace vela {

static_assert(!kProductName.empty());
static_assert(!kVersion.empty());
static_assert(!kToolchainDescription.empty());

}  // namespace vela
