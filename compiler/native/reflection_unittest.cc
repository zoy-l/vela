#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "base/reflection.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/ir/preparation.h"
#include "compiler/native/native_image_data.h"
#include "compiler/project/project_config.h"
#include "compiler/test_support/reflection_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/reflection/reflection_index.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {
namespace {

class ReflectionIndexTest : public ::testing::Test {
 protected:
  void Load(test::ReflectionInput source) {
    const auto config = ParseProjectConfiguration("vela.json", source.configuration);
    ASSERT_TRUE(config) << config.error().message;
    const frontend::SourceInput input{.path = "main.vela", .text = source.source};
    const auto prepared = ir::PrepareParsed(
        frontend::LoadSources(std::span(&input, 1), input.path, config->reflection));
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto data = DescribeNativeImage(prepared.program);
    auto instance = runtime::NativeInstance::Instantiate(
        isolate_, std::shared_ptr<const runtime::NativeImage>(data, data->image()));
    ASSERT_TRUE(instance) << instance.error().message;
    instance_ = std::move(*instance);
    const auto index = runtime::PrepareReflectionIndex(*instance_);
    ASSERT_TRUE(index);
    index_storage_ = *index;
  }
  const runtime::NativeReflectionType* Find(std::string_view name) const {
    return index_storage_->FindType("main.vela", name, {});
  }
  runtime::ReflectionIndex& index() { return *index_storage_; }

 private:
  runtime::Isolate isolate_;
  std::unique_ptr<runtime::NativeInstance> instance_;
  runtime::ReflectionIndex* index_storage_ = nullptr;
};

TEST_F(ReflectionIndexTest, ResolvesInheritanceBeforeRegistrationAndQueryFilters) {
  Load({
      .source = R"(
class Base {
  inherited: int = 7;
  read(): int { return this.inherited; }
}
class Derived extends Base {
  own: int = 1;
  override read(): int { return 3; }
}
)",
      .configuration = R"({"reflection":[
  {"module":"main","type":"Base","members":[
    {"kind":"field","name":"inherited"},{"kind":"method","name":"read"}]},
  {"module":"main","type":"Derived","members":[{"kind":"field","name":"own"}]}
]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  const auto* description = Find("Derived");
  ASSERT_NE(description, nullptr);
  const auto type = index().TypeAt(description->type);
  const auto read = index().FindMember(type, reflection::MemberKind::Method, "read", {});
  ASSERT_TRUE(read);
  EXPECT_EQ(*read, nullptr);
  std::vector<std::string> fields;
  ASSERT_TRUE(index().EnumerateMembers(
      type, reflection::MemberKind::Field, {},
      [&](const auto& member) { fields.emplace_back(member.name.text()); }));
  EXPECT_EQ(fields, (std::vector<std::string>{"inherited", "own"}));
  const auto inherited = index().FindMember(type, reflection::MemberKind::Field, "inherited", {});
  ASSERT_TRUE(inherited);
  ASSERT_NE(*inherited, nullptr);
  const auto* base = Find("Base");
  ASSERT_NE(base, nullptr);
  EXPECT_EQ((*inherited)->declaring_type, base->type);
  const auto declared = index().FindMember(type, reflection::MemberKind::Field, "inherited",
                                           {.scope = reflection::MemberScope::Declared});
  ASSERT_TRUE(declared);
  EXPECT_EQ(*declared, nullptr);
  const auto static_field = index().FindMember(type, reflection::MemberKind::Field, "own",
                                               {.storage = reflection::MemberStorage::Static});
  ASSERT_TRUE(static_field);
  EXPECT_EQ(*static_field, nullptr);
}

TEST_F(ReflectionIndexTest, PropertyVisibilityUsesOnlyRegisteredAccessors) {
  Load({
      .source = R"(
class Counter {
  value: int = 1;
  get amount(): int { return this.value; }
  private set amount(value: int): void { this.value = value; }
}
)",
      .configuration = R"({"reflection":[{"module":"main","type":"Counter","members":[
  {"kind":"setter","name":"amount"}
]}]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  const auto* description = Find("Counter");
  ASSERT_NE(description, nullptr);
  const auto type = index().TypeAt(description->type);
  const auto visible = index().FindMember(type, reflection::MemberKind::Property, "amount", {});
  ASSERT_TRUE(visible);
  EXPECT_EQ(*visible, nullptr);
  const auto private_property =
      index().FindMember(type, reflection::MemberKind::Property, "amount",
                         {.visibility = reflection::VisibilityFilter::Private});
  ASSERT_TRUE(private_property);
  ASSERT_NE(*private_property, nullptr);
  const auto setter = index().ResolveMember((*private_property)->setter);
  ASSERT_TRUE(setter);
  EXPECT_NE((*setter)->flags & reflection::kPrivate, 0U);
  const auto getter = index().ResolveMember((*private_property)->getter);
  ASSERT_FALSE(getter);
  EXPECT_EQ(getter.error(), ExecutionStatus::ReflectionUnavailable);
}

TEST_F(ReflectionIndexTest, EnumeratesSelectedEnumValuesInDeclarationOrder) {
  Load({
      .source = "enum Color { red, green, blue }",
      .configuration = R"({"reflection":[
    {"module":"main","type":"Color","members":[
      {"kind":"enumMember","name":"red"},{"kind":"enumMember","name":"blue"}]}
  ]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  const auto* description = Find("Color");
  ASSERT_NE(description, nullptr);
  std::vector<std::uint32_t> ordinals;
  ASSERT_TRUE(index().EnumerateMembers(
      index().TypeAt(description->type), reflection::MemberKind::EnumMember,
      {.scope = reflection::MemberScope::Declared},
      [&](const auto& member) { ordinals.push_back(member.target); }));
  EXPECT_EQ(ordinals, (std::vector<std::uint32_t>{0, 2}));
  const auto invalid =
      index().FindMember({.constraint = runtime::Heap::TypeOf(runtime::Value::Integer(0))},
                         reflection::MemberKind::EnumMember, "red", {});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), ExecutionStatus::TypeError);
}

TEST_F(ReflectionIndexTest, LooksUpConcreteTypeAndMethodArgumentsWithoutParsingDisplayNames) {
  Load({
      .source = R"(
class Box<T> {
  value: T;
  convert<U>(value: U): U { return value; }
}
)",
      .configuration = R"({"reflection":[
  {"module":"main","type":"Box<int>","members":[
    {"kind":"method","name":"convert","typeArguments":["int"]},
    {"kind":"method","name":"convert","typeArguments":["string"]}]},
  {"module":"main","type":"Box<string>"}
]})",
  });
  ASSERT_FALSE(HasFatalFailure());
  const runtime::ReflectedType integer{
      .constraint = runtime::Heap::TypeOf(runtime::Value::Integer(0)),
  };
  const auto* box = index().FindType("main.vela", "Box", std::span(&integer, 1));
  ASSERT_NE(box, nullptr);
  const auto type = index().TypeAt(box->type);
  const auto method = index().FindMember(type, reflection::MemberKind::Method, "convert", {},
                                         std::span(&integer, 1));
  ASSERT_TRUE(method);
  ASSERT_NE(*method, nullptr);
  const auto missing_arguments =
      index().FindMember(type, reflection::MemberKind::Method, "convert", {});
  ASSERT_TRUE(missing_arguments);
  EXPECT_EQ(*missing_arguments, nullptr);
  EXPECT_EQ(index().FindType("other.vela", "Box", std::span(&integer, 1)), nullptr);
  EXPECT_EQ(index().FindType("main.vela", "Box<int>", {}), nullptr);
}

}  // namespace
}  // namespace vela::compiler::native
