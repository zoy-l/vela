#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>
#include <vela/host.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/development/source_session.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/test_support.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_control.h"
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

  Compiler& compiler() { return *compiler_; }
  runtime::Isolate& isolate() { return isolate_; }

 private:
  std::unique_ptr<Compiler> compiler_;
  SourceSession source_{"main.vela"};
  runtime::Isolate isolate_{1};
};

TEST_P(HostTypeReloadTest, TypeKeepsStructuralMetadataWithoutRetainingAnOldCodeVersion) {
  auto& isolate = this->isolate();
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
    EXPECT_EQ(compiler().resource_counts().images, 1U);
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
  auto& isolate = this->isolate();
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

TEST_P(HostTypeReloadTest, TypedEntriesKeepDefaultedParametersNonNullableAndAllowOmission) {
  ASSERT_TRUE(Update("export func answer(value: int = 42): int { return value; }"));
  auto& isolate = this->isolate();
  auto omitted = Entry<std::int64_t()>::Bind(isolate.native_dispatch(), "main.vela", "answer");
  auto supplied =
      Entry<std::int64_t(std::int64_t)>::Bind(isolate.native_dispatch(), "main.vela", "answer");
  auto nullable = Entry<std::int64_t(Nullable<std::int64_t>)>::Bind(isolate.native_dispatch(),
                                                                    "main.vela", "answer");
  ASSERT_TRUE(omitted);
  ASSERT_TRUE(supplied);
  EXPECT_FALSE(nullable);
  HandleScope scope(isolate);
  const auto first = omitted->Call(scope);
  const auto second = supplied->Call(scope, std::int64_t{21});
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(*first, 42);
  EXPECT_EQ(*second, 21);
}

TEST_P(HostTypeReloadTest, TypedNamedEntryUsesCurrentPublicationAndCaptureKeepsItsVersion) {
  auto& isolate = this->isolate();
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
  EXPECT_EQ(compiler().resource_counts().images, 2U);
  captured = std::unexpected(Error{.message = {}, .stack_trace = {}});
  isolate.heap().Collect();
  EXPECT_EQ(compiler().resource_counts().images, 1U);
}

using IntegerCallback = Function<std::int64_t(std::int64_t)>;

Result<Persistent<IntegerCallback>> SaveIntegerCallback(runtime::Isolate& isolate,
                                                        std::string_view name) {
  auto factory = Entry<IntegerCallback()>::Bind(isolate.native_dispatch(), "main.vela", name);
  if (!factory) {
    return std::unexpected(std::move(factory.error()));
  }
  HandleScope registration(isolate);
  auto callback = factory->Call(registration);
  if (!callback) {
    return std::unexpected(std::move(callback.error()));
  }
  return callback->Persist();
}

TEST_P(HostTypeReloadTest, SavedIntegerCallbackKeepsItsVersionAcrossReloadAndCollection) {
  auto& owner = isolate();
  ASSERT_TRUE(Update("export func callback(): (int) => int { return (n: int): int => n + 1; }"));
  auto old = SaveIntegerCallback(owner, "callback");
  ASSERT_TRUE(old);
  ASSERT_TRUE(Update("export func callback(): (int) => int { return (n: int): int => n + 2; }"));
  owner.heap().Collect();
  auto current = SaveIntegerCallback(owner, "callback");
  ASSERT_TRUE(current);
  EXPECT_EQ(current->Call(std::int64_t{40}), 42);
  EXPECT_EQ(old->Call(std::int64_t{40}), 41);
  const auto overflow = old->Call(std::numeric_limits<std::int64_t>::max());
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().status, ExecutionStatus::IntegerOverflow);
  EXPECT_EQ(old->Call(std::numeric_limits<std::int64_t>::min()),
            std::numeric_limits<std::int64_t>::min() + 1);
  HandleScope scope(owner);
  const auto localized = old->ToLocal(scope);
  ASSERT_TRUE(localized);
  old->Reset();
  owner.heap().Collect();
  EXPECT_EQ(localized->Call(scope, std::int64_t{40}), 41);
  const auto reset = old->Call(std::int64_t{40});
  ASSERT_FALSE(reset);
  EXPECT_EQ(reset.error().status, ExecutionStatus::InvalidEntry);
}

TEST_P(HostTypeReloadTest, SavedCallbacksPreserveEveryIntegerResultAndObserveStopRequests) {
  auto& owner = isolate();
  ASSERT_TRUE(Update("export func identity(): (int) => int { return (n: int): int => n; }"));
  auto callback = SaveIntegerCallback(owner, "identity");
  ASSERT_TRUE(callback);
  constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
  EXPECT_EQ(callback->Call(minimum), minimum);
  const Persistent<IntegerCallback> empty;
  const auto missing = empty.Call(minimum);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().status, ExecutionStatus::InvalidEntry);
  owner.native_control().RequestStop();
  const auto stopped = callback->Call(std::int64_t{41});
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().status, ExecutionStatus::ExecutionStopped);
}

TEST_P(HostTypeReloadTest, SavedIntegerCallbackRetainsItsCapturedState) {
  auto& owner = isolate();
  ASSERT_TRUE(
      Update("export func make(delta: int): (int) => int { "
             "return (n: int): int => n + delta; }"));
  auto factory =
      Entry<IntegerCallback(std::int64_t)>::Bind(owner.native_dispatch(), "main.vela", "make");
  ASSERT_TRUE(factory);
  Persistent<IntegerCallback> saved;
  {
    HandleScope registration(owner);
    const auto callback = factory->Call(registration, std::int64_t{7});
    ASSERT_TRUE(callback);
    auto root = callback->Persist();
    ASSERT_TRUE(root);
    saved = std::move(*root);
  }
  owner.heap().Collect();
  EXPECT_EQ(saved.Call(std::int64_t{35}), 42);
}

TEST_P(HostTypeReloadTest, SavedCallbackReadsAChangedCellAfterReload) {
  auto& owner = isolate();
  const std::string source = R"(
export func callbacks(): ((int) => int)[] {
  let delta = 1;
  const read = (n: int): int => n + delta;
  const write = (n: int): int => { delta = n; return delta; };
  return [read, write];
}
)";
  ASSERT_TRUE(Update(source));
  auto factory =
      Entry<Array<IntegerCallback>()>::Bind(owner.native_dispatch(), "main.vela", "callbacks");
  ASSERT_TRUE(factory);
  Persistent<IntegerCallback> reader;
  Persistent<IntegerCallback> writer;
  {
    HandleScope registration(owner);
    const auto pair = factory->Call(registration);
    ASSERT_TRUE(pair);
    const auto read = pair->Get(registration, 0);
    const auto write = pair->Get(registration, 1);
    ASSERT_TRUE(read);
    ASSERT_TRUE(write);
    auto retained_read = read->Persist();
    auto retained_write = write->Persist();
    ASSERT_TRUE(retained_read);
    ASSERT_TRUE(retained_write);
    reader = std::move(*retained_read);
    writer = std::move(*retained_write);
    const auto moved_from = retained_read->Call(std::int64_t{41});
    ASSERT_FALSE(moved_from);
    EXPECT_EQ(moved_from.error().status, ExecutionStatus::InvalidEntry);
  }
  EXPECT_EQ(reader.Call(std::int64_t{41}), 42);
  ASSERT_TRUE(Update(source + "\nexport func added(): bool { return true; }"));
  EXPECT_EQ(writer.Call(std::int64_t{7}), 7);
  owner.heap().Collect();
  EXPECT_EQ(reader.Call(std::int64_t{35}), 42);
  HandleScope event(owner);
  const auto localized_read = reader.ToLocal(event);
  const auto localized_write = writer.ToLocal(event);
  ASSERT_TRUE(localized_read);
  ASSERT_TRUE(localized_write);
  reader.Reset();
  writer.Reset();
  owner.heap().Collect();
  EXPECT_EQ(localized_write->Call(event, std::int64_t{13}), 13);
  EXPECT_EQ(localized_read->Call(event, std::int64_t{29}), 42);
  owner.native_control().RequestStop();
  const auto stopped = localized_read->Call(event, std::int64_t{29});
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().status, ExecutionStatus::ExecutionStopped);
}

TEST_P(HostTypeReloadTest, TypedEntryChecksChangedSignaturesAndRejectsPrivateLookup) {
  auto& isolate = this->isolate();
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
