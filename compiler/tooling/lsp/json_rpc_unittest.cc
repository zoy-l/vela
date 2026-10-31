#include <ios>
#include <sstream>
#include <string>
#include <string_view>

#include <yyjson.h>

#include "gtest/gtest.h"

#include "compiler/tooling/lsp/json_rpc.h"

namespace vela::tooling {
namespace {

TEST(JsonRpcTest, FramesUtf8ByBytesAndReadsConsecutiveMessages) {
  std::ostringstream wire;
  constexpr std::string_view kFirst = R"({"text":"你好😀"})";
  ASSERT_TRUE(WriteRpcMessage(wire, kFirst));
  ASSERT_TRUE(WriteRpcMessage(wire, "{}"));
  EXPECT_TRUE(
      wire.str().starts_with("Content-Length: " + std::to_string(kFirst.size()) + "\r\n\r\n"));
  std::istringstream input(wire.str());
  const auto first = ReadRpcMessage(input);
  ASSERT_TRUE(first);
  const auto& message = *first;
  if (!message.has_value()) {
    FAIL() << "unexpected EOF";
    return;
  }
  const auto* value = yyjson_obj_get(message->root(), "text");
  EXPECT_STREQ(yyjson_get_str(value), "你好😀");
}

TEST(JsonRpcTest, ReadsConsecutiveFramesAndDistinguishesCleanEof) {
  std::istringstream input("Content-Length: 2\r\n\r\n{}Content-Length: 2\r\n\r\n{}");
  const auto first = ReadRpcMessage(input);
  EXPECT_TRUE(first && first->has_value());
  const auto second = ReadRpcMessage(input);
  EXPECT_TRUE(second && second->has_value());
  const auto end = ReadRpcMessage(input);
  ASSERT_TRUE(end);
  EXPECT_FALSE(end->has_value());
}

TEST(JsonRpcTest, HandlesHeaderNamesCaseInsensitivelyAndSkipsUnknownHeaders) {
  std::istringstream input(
      "content-LENGTH: 2\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n{}");
  const auto result = ReadRpcMessage(input);
  ASSERT_TRUE(result);
  const auto& message = *result;
  if (!message.has_value()) {
    FAIL() << "unexpected EOF";
    return;
  }
  EXPECT_TRUE(yyjson_is_obj(message->root()));
}

class InvalidRpcFrameTest : public testing::TestWithParam<std::string_view> {};

TEST_P(InvalidRpcFrameTest, RejectsBrokenFramingBeforeParsingJson) {
  std::istringstream input{std::string(GetParam())};
  const auto result = ReadRpcMessage(input);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, RpcErrorKind::Transport);
}

INSTANTIATE_TEST_SUITE_P(InputBoundaries,
                         InvalidRpcFrameTest,
                         testing::Values("Content-Length: 2\n\n{}",
                                         "Content-Length: -1\r\n\r\n",
                                         "Content-Length: +2\r\n\r\n{}",
                                         "Content-Length: 67108865\r\n\r\n",
                                         "Content-Length: 999999999999999999999999999999\r\n\r\n",
                                         "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}",
                                         "Content-Length: \r\n\r\n",
                                         "Content-Length: 2x\r\n\r\n{}",
                                         "Bad Header\r\n\r\n",
                                         "\r\n{}",
                                         "Content-Length: 2\r\n\r\n{",
                                         "Content-Length: 2\r\n"));

TEST(JsonRpcTest, JsonErrorsDoNotConsumeTheFollowingFrame) {
  std::istringstream input("Content-Length: 1\r\n\r\n{Content-Length: 2\r\n\r\n{}");
  const auto invalid = ReadRpcMessage(input);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().kind, RpcErrorKind::Json);
  const auto valid = ReadRpcMessage(input);
  EXPECT_TRUE(valid && valid->has_value());
}

TEST(JsonRpcTest, ReportsOutputFailure) {
  std::ostringstream output;
  output.setstate(std::ios::badbit);
  EXPECT_FALSE(WriteRpcMessage(output, "{}"));
}

}  // namespace
}  // namespace vela::tooling
