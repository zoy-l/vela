#include <span>
#include <string_view>

#include "gtest/gtest.h"

#include "base/fingerprint.h"

namespace vela {
namespace {

TEST(FingerprintTest, TextAndByteInputsAgreeAndKeepCanonicalOutput) {
  constexpr std::string_view kContent = "Vela cache content";
  const auto fingerprint = ContentFingerprint(kContent);
  EXPECT_EQ(fingerprint, ContentFingerprint(std::as_bytes(std::span(kContent))));
  EXPECT_NE(fingerprint, ContentFingerprint("Vela cache content!"));
  EXPECT_EQ(ContentFingerprint(std::string_view{}).Hex(), "99aa06d3014798d86001c324468d497f");
}

}  // namespace
}  // namespace vela
