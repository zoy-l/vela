#include <string_view>

#include "gtest/gtest.h"

#include "compiler/build_info.h"

namespace vela {
namespace {

TEST(EnvironmentTest, ProductIdentity) {
  EXPECT_EQ(kProductName, std::string_view("Vela"));
  EXPECT_EQ(kVersion, std::string_view("0.1.0"));
}

TEST(EnvironmentTest, ToolchainDescriptionIsPresent) {
  EXPECT_FALSE(kToolchainDescription.empty());
}

}  // namespace
}  // namespace vela
