#include <string_view>

#include "compiler/build_info.h"

int main() {
  if (vela::kProductName != std::string_view("Vela")) {
    return 1;
  }
  if (vela::kVersion.empty() || vela::kToolchainDescription.empty()) {
    return 1;
  }
  return 0;
}
