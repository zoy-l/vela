#include "runtime/isolate/host_data.h"

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "runtime/isolate/isolate.h"

namespace vela::runtime {
namespace {

TEST(HostDataTest, SeparatesTypesAndIsolates) {
  Isolate first;
  Isolate second;
  int first_value = 1;
  int second_value = 2;
  std::string text = "host";
  EXPECT_EQ(HostData::Find<int>(first), nullptr);
  EXPECT_EQ(HostData::Find<std::string>(first), nullptr);
  const HostData first_data(first, first_value);
  const HostData second_data(second, second_value);
  const HostData text_data(first, text);
  EXPECT_EQ(HostData::Find<int>(first), &first_value);
  EXPECT_EQ(HostData::Find<int>(second), &second_value);
  EXPECT_EQ(HostData::Find<std::string>(first), &text);
  EXPECT_EQ(HostData::Find<std::string>(second), nullptr);
}

TEST(HostDataTest, RestoresOuterDataWhenAnInnerScopeEnds) {
  Isolate isolate;
  int outer_value = 1;
  int inner_value = 2;
  {
    const HostData outer(isolate, outer_value);
    {
      const HostData inner(isolate, inner_value);
      EXPECT_EQ(HostData::Find<int>(isolate), &inner_value);
    }
    EXPECT_EQ(HostData::Find<int>(isolate), &outer_value);
  }
  EXPECT_EQ(HostData::Find<int>(isolate), nullptr);
}

TEST(HostDataTest, UnlinksAnOlderAttachmentWithoutInvalidatingTheCurrentOne) {
  Isolate isolate;
  int older_value = 1;
  int current_value = 2;
  auto older = std::make_unique<HostData>(isolate, older_value);
  {
    const HostData current(isolate, current_value);
    older.reset();
    EXPECT_EQ(HostData::Find<int>(isolate), &current_value);
  }
  EXPECT_EQ(HostData::Find<int>(isolate), nullptr);
}

}  // namespace
}  // namespace vela::runtime
