#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/development_session.h"
#include "compiler/native/native_roots.h"
#include "compiler/source_session.h"
#include "runtime/future.h"
#include "runtime/native_debugger.h"
#include "runtime/native_dispatch.h"
#include "runtime/rooted_value.h"
#include "runtime/string.h"

namespace vela::compiler::jit {
namespace {

struct DebugFrameSnapshot final {
  FunctionIdentity identity;
  std::string name;
  std::string source;
};

struct DebugSnapshot final {
  std::vector<DebugFrameSnapshot> frames;
  bool initializing = false;
};

struct JitDebuggerTest : public testing::TestWithParam<NativeCodeTier>,
                         public runtime::NativeDebugger {
  void SetUp() override { Initialize(native::NativeDebugging::Safepoints); }

  void Initialize(native::NativeDebugging debugging) {
    DevelopmentOptions options;
    options.compilation.tier = GetParam();
    options.compilation.debugging = debugging;
    auto created = DevelopmentSession::Create("main.vela", {}, std::move(options));
    ASSERT_TRUE(created);
    session_ = std::move(*created);
    session_->SetDebugger(this);
  }

  std::expected<DevelopmentCandidate, DevelopmentResult> Prepare(std::string source) const {
    return session_->Prepare(
        {
            .base_revision = session_->snapshot()->revision,
            .changes = {{.path = "main.vela", .text = std::move(source)}},
            .added_roots = {},
        },
        DevelopmentOperation::HotReload);
  }

  void Load(std::string source) const {
    auto candidate = Prepare(std::move(source));
    ASSERT_TRUE(candidate);
    ASSERT_EQ(session_->Submit(std::move(*candidate)).status,
              DevelopmentStatus::WaitingForSafepoint);
    ASSERT_EQ(session_->Pump().status, DevelopmentStatus::Accepted);
  }

  void AtSafepoint(const runtime::NativeDebugContext& context) noexcept override {
    if (paused_) {
      paused_(context);
    }
    session_->isolate().heap().Collect();
    DebugSnapshot snapshot;
    snapshot.initializing = session_->isolate().native_control().IsInitializing();
    for (const auto& frame : context.frames()) {
      snapshot.frames.push_back({
          .identity = frame.identity(),
          .name = std::string(frame.name()),
          .source = std::string(frame.source_text()),
      });
    }
    snapshots_.push_back(std::move(snapshot));
  }

  [[nodiscard]] const DebugSnapshot* Find(std::string_view name, std::uint64_t revision) const {
    const auto found = std::ranges::find_if(snapshots_, [&](const auto& snapshot) {
      const auto& frame = snapshot.frames.front();
      return frame.name == name && frame.identity.revision == revision;
    });
    return found == snapshots_.end() ? nullptr : &*found;
  }

  std::unique_ptr<DevelopmentSession> session_;
  std::function<void(const runtime::NativeDebugContext&)> paused_;
  std::vector<DebugSnapshot> snapshots_;
};

struct JitSourceLineTest : public JitDebuggerTest {
  void SetUp() override { Initialize(native::NativeDebugging::SourceLines); }
};

TEST_P(JitSourceLineTest, ReloadAtAnArithmeticSourceLineRestoresVirtualFieldsWithoutReplay) {
  Load(
      "class Box { value: int; text: string; }\n"
      "export func caller(): int {\n"
      "  const box = Box(value: 38, text: 'a' + 'b');\n"
      "  box.value += 2;\n"
      "  box.value += 1;\n"
      "  return box.value + box.text.length - 1;\n"
      "}\n");
  auto candidate = Prepare(
      "class Box { value: int; text: string; added: int = 2; }\n"
      "export func caller(): int { return 0; }\n");
  ASSERT_TRUE(candidate);
  std::optional<DevelopmentCandidate> pending(std::move(*candidate));
  DevelopmentResult published;
  constexpr std::uint32_t kReloadLine = 5;
  paused_ = [&](const runtime::NativeDebugContext& context) {
    const auto& frame = context.top_frame();
    const auto location = frame.location();
    if (frame.name() == "caller" && location && location->line == kReloadLine) {
      if (auto update = std::exchange(pending, std::nullopt)) {
        (void)session_->Submit(std::move(*update));
        published = session_->Pump();
      }
    }
  };
  const auto value = session_->Invoke("main.vela", "caller");
  ASSERT_TRUE(value) << ExecutionStatusName(value.error());
  EXPECT_FALSE(pending);
  EXPECT_EQ(published.status, DevelopmentStatus::Accepted) << published.message;
  EXPECT_EQ(value->AsInteger(), 42);
  EXPECT_EQ(published.timings.publication.materialized_objects,
            GetParam() == NativeCodeTier::Optimized ? 1U : 0U);
  session_->isolate().heap().Collect();
  EXPECT_EQ(session_->code_resources().batches, 1U);
}

TEST_P(JitDebuggerTest, PublishingWhilePausedPreservesOldFrameSourcesAndCallsTheLatestCode) {
  const std::string old_source =
      "func read(): int { return 40; } "
      "export func caller(): int { return read() + 2; }";
  Load(old_source);
  const std::string new_source =
      "func read(): int { return 100; } "
      "export func caller(): int { return 0; }";
  auto prepared = Prepare(new_source);
  ASSERT_TRUE(prepared);
  std::optional<DevelopmentCandidate> pending(std::move(*prepared));
  DevelopmentResult committed;
  paused_ = [&](const runtime::NativeDebugContext&) {
    if (auto candidate = std::exchange(pending, std::nullopt)) {
      (void)session_->Submit(std::move(*candidate));
      committed = session_->Pump();
    }
  };
  const auto value = session_->Invoke("main.vela", "caller");
  ASSERT_TRUE(value) << static_cast<std::uint32_t>(value.error());
  EXPECT_EQ(value->AsInteger(), 102);
  EXPECT_EQ(committed.status, DevelopmentStatus::Accepted);
  const auto* read = Find("read", 2);
  ASSERT_NE(read, nullptr);
  ASSERT_EQ(read->frames.size(), 2U);
  EXPECT_EQ(read->frames.front().source, new_source);
  EXPECT_EQ(read->frames.back().source, old_source);
  EXPECT_EQ(read->frames.back().identity.revision, 1U);
  session_->isolate().heap().Collect();
  EXPECT_EQ(session_->code_resources().batches, 1U);
}

TEST_P(JitDebuggerTest, InitializationCanPauseButPublicationWaitsUntilItsStateIsComplete) {
  Load(
      "let value = initial(); func initial(): int { return 42; } "
      "export func read(): int { return value; }");
  auto prepared = Prepare(
      "let value = initial(); func initial(): int { return 100; } "
      "export func read(): int { return value + 1; }");
  ASSERT_TRUE(prepared);
  std::optional<DevelopmentCandidate> pending(std::move(*prepared));
  DevelopmentResult while_initializing;
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.frames().front().name() == "initial") {
      if (auto candidate = std::exchange(pending, std::nullopt)) {
        (void)session_->Submit(std::move(*candidate));
        while_initializing = session_->Pump();
      }
    }
  };
  const auto old = session_->Invoke("main.vela", "read");
  ASSERT_TRUE(old);
  EXPECT_EQ(old->AsInteger(), 42);
  EXPECT_EQ(while_initializing.status, DevelopmentStatus::WaitingForSafepoint);
  EXPECT_EQ(while_initializing.revision, 1U);
  const auto* initial = Find("initial", 1);
  ASSERT_NE(initial, nullptr);
  EXPECT_TRUE(initial->initializing);
  const auto current = session_->Invoke("main.vela", "read");
  ASSERT_TRUE(current);
  EXPECT_EQ(current->AsInteger(), 43);
}

TEST_P(JitDebuggerTest, PausedInitializerConsumesControlCommandsBeforeItCanPublish) {
  Load(
      "let value = initial(); func initial(): int { return 42; } "
      "export func read(): int { return value; }");
  bool consumed = false;
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.frames().front().name() != "initial") {
      return;
    }
    ASSERT_TRUE(session_->safepoint_endpoint().Post([&] { consumed = true; }));
    (void)session_->Pump();
    EXPECT_TRUE(consumed);
  };
  const auto result = session_->Invoke("main.vela", "read");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), 42);
  EXPECT_TRUE(consumed);
}

TEST_P(JitDebuggerTest, DeletingAPastFieldAccessKeepsAnActiveCallersRootedLocalValue) {
  Load(
      "class Box { value: string = 'retained local'; } "
      "func checkpoint(): void {} "
      "export func make(): Box { return Box(); } "
      "export func read(box: Box): string { const value = box.value; checkpoint(); return value; "
      "}");
  const auto made = session_->Invoke("main.vela", "make");
  ASSERT_TRUE(made);
  const runtime::RootedValue box(session_->isolate(), *made);
  auto candidate = Prepare(
      "class Box {} func checkpoint(): void {} "
      "export func make(): Box { return Box(); } "
      "export func read(box: Box): string { return 'new'; }");
  ASSERT_TRUE(candidate);
  std::optional<DevelopmentCandidate> pending(std::move(*candidate));
  DevelopmentResult published;
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.top_frame().name() == "checkpoint") {
      if (auto update = std::exchange(pending, std::nullopt)) {
        (void)session_->Submit(std::move(*update));
        published = session_->Pump();
      }
    }
  };
  const auto old = session_->Invoke("main.vela", "read", std::array{box.value()});
  ASSERT_TRUE(old);
  EXPECT_EQ(published.status, DevelopmentStatus::Accepted);
  EXPECT_EQ(old->AsString()->value(), "retained local");
  const auto current = session_->Invoke("main.vela", "read", std::array{box.value()});
  ASSERT_TRUE(current);
  EXPECT_EQ(current->AsString()->value(), "new");
}

TEST_P(JitDebuggerTest, AnAlreadyResolvedMethodCallDoesNotRetainItsOldLookupBinding) {
  Load(
      "func checkpoint(): void {} "
      "class Box { read(): int { checkpoint(); return 42; } } "
      "export func make(): Box { return Box(); } "
      "export func invoke(box: Box): int { return box.read(); }");
  const auto made = session_->Invoke("main.vela", "make");
  ASSERT_TRUE(made);
  const runtime::RootedValue box(session_->isolate(), *made);
  auto candidate = Prepare(
      "func checkpoint(): void {} "
      "class Box { static read(): int { return 100; } } "
      "export func make(): Box { return Box(); } "
      "export func invoke(box: Box): int { return Box.read(); }");
  ASSERT_TRUE(candidate);
  std::optional<DevelopmentCandidate> pending(std::move(*candidate));
  DevelopmentResult published;
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.top_frame().name() == "checkpoint") {
      if (auto update = std::exchange(pending, std::nullopt)) {
        (void)session_->Submit(std::move(*update));
        published = session_->Pump();
      }
    }
  };
  const auto old = session_->Invoke("main.vela", "invoke", std::array{box.value()});
  ASSERT_TRUE(old);
  EXPECT_EQ(published.status, DevelopmentStatus::Accepted);
  EXPECT_EQ(old->AsInteger(), 42);
  const auto current = session_->Invoke("main.vela", "invoke", std::array{box.value()});
  ASSERT_TRUE(current);
  EXPECT_EQ(current->AsInteger(), 100);
}

TEST_P(JitDebuggerTest, DeoptimizationDoesNotRepeatAnEnteredCallOrRunTheNewCallerBody) {
  Load(
      "let calls = 0; func checkpoint(): void {} "
      "func leaf(): int { calls += 1; checkpoint(); return 40; } "
      "func read(): int { return 1; } "
      "export func caller(): int { const value = leaf(); return value + read(); } "
      "export func count(): int { return calls; }");
  auto candidate = Prepare(
      "let calls = 0; func checkpoint(): void {} func leaf(): int { calls += 100; return 100; } "
      "func read(): int { return 2; } export func caller(): int { return 0; } "
      "export func count(): int { return calls; }");
  ASSERT_TRUE(candidate);
  std::optional<DevelopmentCandidate> pending(std::move(*candidate));
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.top_frame().name() == "checkpoint") {
      if (auto update = std::exchange(pending, std::nullopt)) {
        (void)session_->Submit(std::move(*update));
        EXPECT_EQ(session_->Pump().status, DevelopmentStatus::Accepted);
      }
    }
  };
  const auto value = session_->Invoke("main.vela", "caller");
  ASSERT_TRUE(value);
  EXPECT_EQ(value->AsInteger(), 42);
  const auto calls = session_->Invoke("main.vela", "count");
  ASSERT_TRUE(calls);
  EXPECT_EQ(calls->AsInteger(), 1);
  EXPECT_EQ(session_->isolate().native_dispatch().deoptimized_frames() != 0,
            GetParam() == NativeCodeTier::Optimized);
}

TEST_P(JitDebuggerTest, DeoptimizationPreservesThePendingExceptionAtTheOutermostRecovery) {
  Load(
      "class Failure { value: int; } func checkpoint(): void {} "
      "func leaf(): int { checkpoint(); throw Failure(value: 40); } "
      "func read(): int { return 1; } export func caller(): int { try { return leaf(); } "
      "catch (error: Failure) { return error.value + read(); } }");
  auto candidate = Prepare(
      "class Failure { value: int; } func checkpoint(): void {} "
      "func leaf(): int { return 100; } func read(): int { return 2; } "
      "export func caller(): int { return 0; }");
  ASSERT_TRUE(candidate);
  std::optional<DevelopmentCandidate> pending(std::move(*candidate));
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.top_frame().name() == "checkpoint") {
      if (auto update = std::exchange(pending, std::nullopt)) {
        (void)session_->Submit(std::move(*update));
        EXPECT_EQ(session_->Pump().status, DevelopmentStatus::Accepted);
      }
    }
  };
  const auto value = session_->Invoke("main.vela", "caller");
  ASSERT_TRUE(value) << static_cast<std::uint32_t>(value.error());
  EXPECT_EQ(value->AsInteger(), 42);
}

TEST_P(JitDebuggerTest, ResumedAsyncFrameCanDeoptimizeAndSuspendAgainInItsOriginalVersion) {
  Load(
      "func checkpoint(): void {} func read(): int { return 1; } "
      "async func task(): Future<int> { const saved = 40; await sleep(1); checkpoint(); "
      "await sleep(1); return saved + read(); } "
      "export func start(): Future<int> { return task(); }");
  auto candidate = Prepare(
      "func checkpoint(): void {} func read(): int { return 2; } "
      "async func task(): Future<int> { return 100; } "
      "export func start(): Future<int> { return task(); }");
  ASSERT_TRUE(candidate);
  std::optional<DevelopmentCandidate> pending(std::move(*candidate));
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.top_frame().name() == "checkpoint") {
      if (auto update = std::exchange(pending, std::nullopt)) {
        (void)session_->Submit(std::move(*update));
        EXPECT_EQ(session_->Pump().status, DevelopmentStatus::Accepted);
      }
    }
  };
  {
    const auto value = session_->Invoke("main.vela", "start");
    ASSERT_TRUE(value);
    const runtime::RootedValue root(session_->isolate(), *value);
    auto* future = runtime::Heap::GetFuture(*root.value().AsObject());
    ASSERT_NE(future, nullptr);
    ASSERT_TRUE(
        session_->isolate().scheduler().PumpUntil(*future, runtime::FuturePumpMode::WaitForHost));
    const auto completed_value = future->value();
    if (!completed_value) {
      FAIL() << "Future did not complete with a value";
    }
    EXPECT_EQ(completed_value->AsInteger(), 42);
  }
  session_->isolate().heap().Collect();
  EXPECT_EQ(session_->code_resources().batches, 1U);
}

TEST_P(JitDebuggerTest, SuspendedContinuationAndCapturedEntrySelectTheirOriginalBaseline) {
  Load(
      "func read(): int { return 1; } "
      "export async func task(): Future<int> { const text = ['4', '0'].join(''); "
      "await sleep(1); return int.parse(text) + read(); }");
  auto entry = session_->isolate().native_dispatch().FindEntry("main.vela", "task");
  ASSERT_TRUE(entry);
  {
    auto captured = entry->Capture();
    ASSERT_TRUE(captured);
    const auto started = captured->Invoke({});
    ASSERT_TRUE(started);
    const runtime::RootedValue root(session_->isolate(), *started);
    Load("func read(): int { return 2; } export async func task(): Future<int> { return 100; }");
    auto* future = runtime::Heap::GetFuture(*root.value().AsObject());
    ASSERT_NE(future, nullptr);
    ASSERT_TRUE(
        session_->isolate().scheduler().PumpUntil(*future, runtime::FuturePumpMode::WaitForHost));
    const auto completed_value = future->value();
    if (!completed_value) {
      FAIL() << "Future did not complete with a value";
    }
    EXPECT_EQ(completed_value->AsInteger(), 42);
    const auto restarted = captured->Invoke({});
    ASSERT_TRUE(restarted);
    const runtime::RootedValue restarted_root(session_->isolate(), *restarted);
    auto* old_future = runtime::Heap::GetFuture(*restarted_root.value().AsObject());
    ASSERT_NE(old_future, nullptr);
    ASSERT_TRUE(session_->isolate().scheduler().PumpUntil(*old_future,
                                                          runtime::FuturePumpMode::WaitForHost));
    const auto old_value = old_future->value();
    if (!old_value) {
      FAIL() << "Original continuation did not complete with a value";
    }
    EXPECT_EQ(old_value->AsInteger(), 42);
    const auto latest = entry->Invoke({});
    ASSERT_TRUE(latest);
    auto* current_future = runtime::Heap::GetFuture(*latest->AsObject());
    ASSERT_NE(current_future, nullptr);
    const auto latest_value = current_future->value();
    if (!latest_value) {
      FAIL() << "Current continuation did not complete with a value";
    }
    EXPECT_EQ(latest_value->AsInteger(), 100);
  }
  session_->isolate().heap().Collect();
  EXPECT_EQ(session_->code_resources().batches, 1U);
}

TEST_P(JitDebuggerTest, LargeFramesRestoreFromStorageReservedBeforePublication) {
  constexpr std::int64_t kLocals = 128;
  std::string source =
      "func checkpoint(): void {} func read(): int { return 1; } "
      "export func caller(input: int): int { ";
  std::string sum;
  for (std::int64_t index = 0; index < kLocals; ++index) {
    const auto name = "local" + std::to_string(index);
    source += "const " + name + " = input + " + std::to_string(index) + "; ";
    sum += (sum.empty() ? "" : " + ") + name;
  }
  Load(source + "checkpoint(); return " + sum + " + read(); }");
  auto candidate = Prepare(
      "func checkpoint(): void {} func read(): int { return 2; } "
      "export func caller(input: int): int { return 100; }");
  ASSERT_TRUE(candidate);
  std::optional<DevelopmentCandidate> pending(std::move(*candidate));
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.top_frame().name() == "checkpoint") {
      if (auto update = std::exchange(pending, std::nullopt)) {
        (void)session_->Submit(std::move(*update));
        EXPECT_EQ(session_->Pump().status, DevelopmentStatus::Accepted);
        for (const auto& frame : context.frames()) {
          EXPECT_EQ(frame.tier(), NativeCodeTier::Baseline);
        }
      }
    }
  };
  const auto value =
      session_->Invoke("main.vela", "caller", std::array{runtime::Value::Integer(40)});
  ASSERT_TRUE(value) << static_cast<std::uint32_t>(value.error());
  EXPECT_EQ(value->AsInteger(), (kLocals * 40) + (kLocals * (kLocals - 1) / 2) + 2);
}

TEST_P(JitDebuggerTest, RejectedPublicationLeavesOptimizationAndLogicalFramesUnchanged) {
  Load(
      "class Box { value: int = 42; } func checkpoint(): void {} "
      "export func make(): Box { return Box(); } "
      "export func caller(box: Box): int { checkpoint(); return box.value; }");
  const auto made = session_->Invoke("main.vela", "make");
  ASSERT_TRUE(made);
  const runtime::RootedValue root(session_->isolate(), *made);
  auto candidate = Prepare(
      "class Box {} func checkpoint(): void {} "
      "export func make(): Box { return Box(); } "
      "export func caller(box: Box): int { return 0; }");
  ASSERT_TRUE(candidate);
  std::optional<DevelopmentCandidate> pending(std::move(*candidate));
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.top_frame().name() == "checkpoint") {
      if (auto update = std::exchange(pending, std::nullopt)) {
        (void)session_->Submit(std::move(*update));
        EXPECT_EQ(session_->Pump().status, DevelopmentStatus::RequiresReload);
        EXPECT_EQ(context.top_frame().tier(), GetParam());
      }
    }
  };
  const auto value = session_->Invoke("main.vela", "caller", std::array{root.value()});
  ASSERT_TRUE(value);
  EXPECT_EQ(value->AsInteger(), 42);
  EXPECT_EQ(session_->snapshot()->revision, 1U);
  EXPECT_EQ(session_->isolate().native_dispatch().deoptimized_frames(), 0U);
}

TEST_P(JitDebuggerTest, VirtualFieldsAreMaterializedBeforeMigrationAndResumeTheOriginalCaller) {
  const std::string common =
      "let calls = 0; let defaults = 0; func initial(): int { defaults++; return 2; } "
      "func checkpoint(): void {} export func count(): int { return calls; } "
      "export func initialized(): int { return defaults; } ";
  Load(common +
       "class Box { value: int; text: string; read(): int { return this.value + this.text.length; "
       "} } "
       "export func caller(): int { calls++; const box = Box(value: 38, text: 'o' + 'k'); "
       "checkpoint(); return box.read(); }");
  auto candidate = Prepare(common +
                           "class Box { value: int; text: string; added: int = initial(); "
                           "read(): int { return this.value + this.text.length + this.added; } } "
                           "export func caller(): int { return 0; }");
  ASSERT_TRUE(candidate);
  std::optional<DevelopmentCandidate> pending(std::move(*candidate));
  DevelopmentResult published;
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.top_frame().name() == "checkpoint") {
      if (auto update = std::exchange(pending, std::nullopt)) {
        (void)session_->Submit(std::move(*update));
        published = session_->Pump();
      }
    }
  };
  const auto value = session_->Invoke("main.vela", "caller");
  ASSERT_TRUE(value) << ExecutionStatusName(value.error());
  EXPECT_EQ(published.status, DevelopmentStatus::Accepted) << published.message;
  EXPECT_EQ(value->AsInteger(), 42);
  EXPECT_EQ(published.timings.publication.materialized_objects,
            GetParam() == NativeCodeTier::Optimized ? 1U : 0U);
  EXPECT_EQ(session_->Invoke("main.vela", "count")->AsInteger(), 1);
  EXPECT_EQ(session_->Invoke("main.vela", "initialized")->AsInteger(), 1);
  EXPECT_EQ(session_->Invoke("main.vela", "caller")->AsInteger(), 0);
  session_->isolate().heap().Collect();
  EXPECT_EQ(session_->code_resources().batches, 1U);
}

TEST_P(JitDebuggerTest, VirtualInstancesParticipateInRequiredFieldConflictChecks) {
  Load(
      "func checkpoint(): void {} class Box { value: int; read(): int { return this.value; } } "
      "export func caller(): int { const box = Box(value: 42); checkpoint(); return box.read(); }");
  auto candidate = Prepare(
      "func checkpoint(): void {} class Box { value: int; required: int; "
      "read(): int { return this.value; } } export func caller(): int { return 0; }");
  ASSERT_TRUE(candidate);
  std::optional<DevelopmentCandidate> pending(std::move(*candidate));
  DevelopmentResult published;
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.top_frame().name() == "checkpoint") {
      if (auto update = std::exchange(pending, std::nullopt)) {
        (void)session_->Submit(std::move(*update));
        published = session_->Pump();
      }
    }
  };
  const auto value = session_->Invoke("main.vela", "caller");
  ASSERT_TRUE(value) << ExecutionStatusName(value.error());
  EXPECT_EQ(published.status, DevelopmentStatus::RequiresReload) << published.message;
  EXPECT_EQ(value->AsInteger(), 42);
  EXPECT_EQ(session_->snapshot()->revision, 1U);
  EXPECT_EQ(session_->isolate().native_dispatch().deoptimized_frames(), 0U);
  session_->isolate().heap().Collect();
  EXPECT_EQ(session_->code_resources().batches, 1U);
}

TEST_P(JitDebuggerTest, MaterializationPreservesSharedChildrenAndCyclesAcrossReload) {
  Load(
      "func checkpoint(): void {} class Link { value: int; next: Link?; other: Link?; "
      "read(): int { return this.value; } } export func caller(): int { "
      "const first = Link(value: 20, next: null, other: null); "
      "const second = Link(value: 20, next: first, other: first); "
      "first.next = second; first.other = second; checkpoint(); "
      "const next = first.next as Link; return next.next == first && first.other == next "
      "? first.read() + next.value : 0; }");
  auto candidate = Prepare(
      "func checkpoint(): void {} class Link { value: int; next: Link?; other: Link?; "
      "added: int = 2; read(): int { return this.value + this.added; } } "
      "export func caller(): int { return 0; }");
  ASSERT_TRUE(candidate);
  std::optional<DevelopmentCandidate> pending(std::move(*candidate));
  DevelopmentResult published;
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.top_frame().name() == "checkpoint") {
      if (auto update = std::exchange(pending, std::nullopt)) {
        (void)session_->Submit(std::move(*update));
        published = session_->Pump();
      }
    }
  };
  const auto value = session_->Invoke("main.vela", "caller");
  ASSERT_TRUE(value) << ExecutionStatusName(value.error());
  EXPECT_EQ(published.status, DevelopmentStatus::Accepted) << published.message;
  EXPECT_EQ(value->AsInteger(), 42);
  EXPECT_EQ(published.timings.publication.materialized_objects,
            GetParam() == NativeCodeTier::Optimized ? 2U : 0U);
  session_->isolate().heap().Collect();
  EXPECT_EQ(session_->code_resources().batches, 1U);
}

TEST_P(JitDebuggerTest, FailedMaterializationPreparationLeavesTheOldVirtualFrameRunnable) {
  Load(
      "func checkpoint(): void {} class Box { value: int; } "
      "export func caller(): int { const box = Box(value: 42); checkpoint(); return box.value; }");
  auto candidate = Prepare(
      "func checkpoint(): void {} class Box { value: int; added: int = 2; } "
      "export func caller(): int { return 0; }");
  ASSERT_TRUE(candidate);
  std::optional<DevelopmentCandidate> pending(std::move(*candidate));
  DevelopmentResult published;
  paused_ = [&](const runtime::NativeDebugContext& context) {
    if (context.top_frame().name() == "checkpoint") {
      if (auto update = std::exchange(pending, std::nullopt)) {
        auto& heap = session_->isolate().heap();
        heap.Collect();
        heap.SetAllocationLimit(heap.allocated_bytes());
        (void)session_->Submit(std::move(*update));
        published = session_->Pump();
        heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
      }
    }
  };
  const auto value = session_->Invoke("main.vela", "caller");
  ASSERT_TRUE(value) << ExecutionStatusName(value.error());
  EXPECT_EQ(published.status, DevelopmentStatus::InstantiationFailed) << published.message;
  EXPECT_EQ(value->AsInteger(), 42);
  EXPECT_EQ(session_->snapshot()->revision, 1U);
  EXPECT_EQ(session_->isolate().native_dispatch().deoptimized_frames(), 0U);
  session_->isolate().heap().Collect();
  EXPECT_EQ(session_->code_resources().batches, 1U);
}

INSTANTIATE_TEST_SUITE_P(Tiers,
                         JitDebuggerTest,
                         testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));
INSTANTIATE_TEST_SUITE_P(Tiers,
                         JitSourceLineTest,
                         testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

}  // namespace
}  // namespace vela::compiler::jit
