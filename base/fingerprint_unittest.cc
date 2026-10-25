#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "base/fingerprint.h"

namespace vela {
namespace {

TEST(FingerprintTest, IsDeterministicContentSensitiveAndCanonical) {
  const std::string value = "Vela cache content";
  const auto first = ContentFingerprint(value);
  EXPECT_EQ(first, ContentFingerprint(std::string_view(value)));
  EXPECT_NE(first, ContentFingerprint(value + "!"));
  EXPECT_EQ(first.Hex().size(), 2U * sizeof(Fingerprint));
  EXPECT_EQ(ContentFingerprint(std::string_view()).Hex(), "99aa06d3014798d86001c324468d497f");
}

}  // namespace
}  // namespace vela
