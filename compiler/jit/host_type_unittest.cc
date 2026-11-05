#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>
#include <vela/host.h>

#include "base/native_program_mode.h"
#include "compiler/development/source_session.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/test_support.h"
#include "runtime/isolate/isolate.h"
#include "vela/collections.h"
#include "vela/entry.h"
#include "vela/error.h"
#include "vela/handles.h"
#include "vela/type.h"
#include "vela/types.h"

namespace vela {
namespace {
struct ReloadBoxTag;
struct ReloadViewTag;
}  // namespace

template <>
struct ScriptTypeTraits<ReloadBoxTag> final {
  static constexpr auto kKind = ScriptTypeKind::Class;
  static constexpr std::string_view kName = "Box";
  static constexpr std::string_view kModule = "main.vela";
  using Arguments = std::tuple<>;
};
template <>
struct ScriptTypeTraits<ReloadViewTag> final {
  static constexpr auto kKind = ScriptTypeKind::Interface;
  static constexpr std::string_view kName = "View";
  static constexpr std::string_view kModule = "main.vela";
  using Arguments = std::tuple<>;
};

namespace compiler::jit {
namespace {

class HostTypeReloadTest : public testing::TestWithParam<NativeCodeTier> {
 protected:
  void SetUp() override {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler);
    compiler_ = std::move(*compiler);
  }

  std::expected<void, std::string> Update(std::string text) {
    const auto retained = isolate_.native_dispatch().RetainedDeclarations();
    return test::UpdateSource(*compiler_, source_, isolate_, std::move(text), GetParam(), retained);
  }

  runtime::Isolate& isolate_state() { return isolate_; }
  const Compiler& compiler_state() const { return *compiler_; }

 private:
  std::unique_ptr<Compiler> compiler_;
  SourceSession source_{"main.vela"};
  runtime::Isolate isolate_{1};
};

TEST_P(HostTypeReloadTest, TypeKeepsStructuralMetadataWithoutRetainingAnOldCodeVersion) {
  auto& isolate = isolate_state();
  const std::string source =
      "class Box { value: int = 42; } export func answer(): int { return Box().value; }";
  auto updated = Update(source);
  ASSERT_TRUE(updated) << updated.error();
  isolate.heap().Collect();
  const auto initial = isolate.layout_counts();
  using Table = Map<Object<ReloadBoxTag>, Function<std::int64_t(Object<ReloadBoxTag>)>>;
  {
    const auto type = Type<Table>::Bind(isolate.native_dispatch());
    ASSERT_TRUE(type);
    const auto retained = isolate.layout_counts();
    EXPECT_GT(retained.collections, initial.collections);
    EXPECT_GT(retained.signatures, initial.signatures);
    updated = Update(source);
    ASSERT_TRUE(updated) << updated.error();
    isolate.heap().Collect();
    EXPECT_EQ(compiler_state().resource_counts().images, 1U);
    EXPECT_EQ(isolate.layout_counts().collections, retained.collections);
    EXPECT_EQ(isolate.layout_counts().signatures, retained.signatures);
    {
      HandleScope scope(isolate);
      const auto table = CreateMap(scope, *type);
      ASSERT_TRUE(table);
      EXPECT_EQ(table->Size(), 0);
    }
    isolate.heap().Collect();
    EXPECT_FALSE(isolate.TryStop());
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.layout_counts(), initial);
}

TEST_P(HostTypeReloadTest, AContainerRetainsItsMetadataAfterItsExplicitTypeIsReleased) {
  auto& isolate = isolate_state();
  auto updated = Update(
      "interface View { read(): int; } "
      "class Box implements View { read(): int { return 42; } } "
      "export func answer(): int { return Box().read(); }");
  ASSERT_TRUE(updated) << updated.error();
  isolate.heap().Collect();
  const auto initial = isolate.layout_counts();
  using Views = Array<Nullable<Object<ReloadViewTag>>>;
  {
    HandleScope scope(isolate);
    std::optional<Local<Views>> values;
    {
      auto type = Type<Views>::Bind(isolate.native_dispatch());
      ASSERT_TRUE(type);
      const auto created = CreateArray(scope, *type);
      ASSERT_TRUE(created);
      values = *created;
    }
    isolate.heap().Collect();
    EXPECT_GT(isolate.layout_counts().arrays, initial.arrays);
    ASSERT_TRUE(values.has_value());
    ASSERT_TRUE(values->Push(scope, std::nullopt));
    const auto null = values->Get(scope, 0);
    ASSERT_TRUE(null);
    EXPECT_FALSE(*null);
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.layout_counts(), initial);
}

TEST_P(HostTypeReloadTest, TypedNamedEntryUsesCurrentPublicationAndCaptureKeepsItsVersion) {
  auto& isolate = isolate_state();
  ASSERT_TRUE(Update("export func answer(): int { return 41; }"));
  auto entry = Entry<std::int64_t()>::Bind(isolate.native_dispatch(), "main.vela", "answer");
  ASSERT_TRUE(entry);
  auto captured = entry->Capture();
  ASSERT_TRUE(captured);
  ASSERT_TRUE(Update("export func answer(): int { return 42; }"));
  isolate.heap().Collect();
  {
    HandleScope scope(isolate);
    const auto current = entry->Call(scope);
    const auto old = captured->Call(scope);
    ASSERT_TRUE(current);
    ASSERT_TRUE(old);
    EXPECT_EQ(*current, 42);
    EXPECT_EQ(*old, 41);
  }
  EXPECT_EQ(compiler_state().resource_counts().images, 2U);
  captured = std::unexpected(Error{.message = {}, .stack_trace = {}});
  isolate.heap().Collect();
  EXPECT_EQ(compiler_state().resource_counts().images, 1U);
}

TEST_P(HostTypeReloadTest, TypedEntryChecksChangedSignaturesAndRejectsPrivateLookup) {
  auto& isolate = isolate_state();
  ASSERT_TRUE(
      Update("func hidden(): int { return 41; } export func answer(): int { return hidden(); }"));
  const auto hidden = Entry<std::int64_t()>::Bind(isolate.native_dispatch(), "main.vela", "hidden");
  EXPECT_FALSE(hidden);
  auto entry = Entry<std::int64_t()>::Bind(isolate.native_dispatch(), "main.vela", "answer");
  ASSERT_TRUE(entry);
  EXPECT_FALSE(Update("export func answer(): string { return 'changed'; }"));
  HandleScope scope(isolate);
  const auto changed = entry->Call(scope);
  ASSERT_TRUE(changed);
  EXPECT_EQ(*changed, 41);
}

INSTANTIATE_TEST_SUITE_P(NativeTiers,
                         HostTypeReloadTest,
                         testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

}  // namespace
}  // namespace compiler::jit
}  // namespace vela
