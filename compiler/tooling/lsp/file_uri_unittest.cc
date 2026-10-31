#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "base/path.h"
#include "compiler/tooling/lsp/file_uri.h"

namespace vela::tooling {
namespace {

#ifdef _WIN32
constexpr std::string_view kRoot = "C:/folder/";
#else
constexpr std::string_view kRoot = "/folder/";
#endif

TEST(FileUriTest, RoundTripsSpacesUnicodeAndReservedPathCharacters) {
  const auto path = Utf8Path(std::string(kRoot) + "空 格😀#?.vela");
  const auto uri = FileUri(path);
  EXPECT_TRUE(uri.contains("%20"));
  EXPECT_TRUE(uri.contains("%23%3F"));
  const auto decoded = FileUriPath(uri);
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(*decoded, path);
}

TEST(FileUriTest, AcceptsLocalhostAndCaseInsensitiveScheme) {
  const auto path = Utf8Path(std::string(kRoot) + "main.vela");
  const auto uri = FileUri(path);
  constexpr std::string_view kScheme = "file://";
  const auto local = FileUriPath("FILE://LOCALHOST" + uri.substr(kScheme.size()));
  ASSERT_TRUE(local) << local.error();
  EXPECT_EQ(*local, path);
}

class InvalidFileUriTest : public testing::TestWithParam<std::string_view> {};
TEST_P(InvalidFileUriTest, RejectsMalformedOrNonFileIdentifiers) {
  EXPECT_FALSE(FileUriPath(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(InputBoundaries,
                         InvalidFileUriTest,
                         testing::Values("relative.vela",
                                         "untitled:main.vela",
                                         "file://host",
                                         "file:///C:/bad%",
                                         "file:///C:/bad%gg",
                                         "file:///C:/bad%00",
                                         "file:///C:/bad%ff",
                                         "file:///C:/main.vela?query",
                                         "file:///C:/main.vela#fragment",
                                         "file://user@host/share/main.vela",
                                         "file://host%2fother/share/main.vela"));

#ifdef _WIN32
TEST(FileUriTest, PreservesUncServerAndShareIdentity) {
  constexpr std::string_view kUri = "file://server/share/folder/main.vela";
  const auto path = FileUriPath(kUri);
  ASSERT_TRUE(path) << path.error();
  EXPECT_EQ(FileUri(*path), kUri);
  EXPECT_FALSE(FileUriPath("file:///relative/main.vela"));
}
#endif

}  // namespace
}  // namespace vela::tooling
