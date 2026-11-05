#include "runtime/isolate/member_table.h"

#include <array>
#include <utility>

#include "base/member.h"
#include "gtest/gtest.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

TEST(MemberTableTest, CanonicalReceiverWinsAcrossNominalLayoutsAndMoves) {
  Isolate isolate;
  auto* nominal = isolate.CreateNominalType({.interfaces = {}, .declaration = {}});
  const ClassLayoutOptions options{
      .callable = {},
      .interfaces = {},
      .dispatch_targets = {},
      .nominal = nominal,
      .field_declarations = {},
  };
  const auto* first = isolate.CreateClassLayout(0, {}, options);
  const auto* second = isolate.CreateClassLayout(0, {}, options);
  const auto type = TypeConstraint{.kind = ValueKind::Object, .nominal = nominal};
  const std::array types{type, type, TypeConstraint{.kind = ValueKind::Integer}};
  MemberTable table(types, {
                               {.name = "value"},
                               {.receiver_type = 1, .name = "duplicate"},
                               {.receiver_type = 2, .name = "scalar", .kind = MemberKind::Method},
                           });
  const auto* member = table.Find(type, "value");
  ASSERT_NE(member, nullptr);
  auto moved = std::move(table);
  const std::array receivers{
      Value::Object(isolate.heap().AllocateInstance(*first)),
      Value::Object(isolate.heap().AllocateInstance(*second)),
  };
  const RootRange roots(isolate.heap(), receivers.data(), receivers.size());
  for (const auto receiver : receivers) {
    EXPECT_EQ(moved.Find(receiver, "value"), member);
    EXPECT_EQ(moved.Find(receiver, "duplicate"), nullptr);
    EXPECT_EQ(moved.Find(receiver, "missing"), nullptr);
  }
  EXPECT_NE(moved.Find(Value::Integer(0), "scalar"), nullptr);
}

}  // namespace
}  // namespace vela::runtime
