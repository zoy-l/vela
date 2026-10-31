#include "compiler/jit/test_support.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "gtest/gtest.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/rooted_value.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_image.h"
#include "runtime/value/value.h"

namespace vela::compiler::jit::test {

std::expected<SourceCandidate, SourceCandidateError> PrepareText(
    const SourceSession& source,
    std::string text,
    std::span<const DeclarationId> retained) {
  return source.Prepare(
      {
          .base_revision = source.snapshot()->revision,
          .changes = {{.path = "main.vela", .text = std::move(text)}},
          .added_roots = {},
      },
      retained);
}

std::expected<CompiledImage, frontend::Diagnostics> CompileText(Compiler& compiler,
                                                                std::string text,
                                                                NativeCodeTier tier) {
  SourceSession source("main.vela");
  const auto candidate = PrepareText(source, std::move(text));
  if (!candidate) {
    return std::unexpected(candidate.error().diagnostics);
  }
  return compiler.Compile(candidate->snapshot().program, {.tier = tier, .preserve_ir = true});
}

std::expected<CompiledUpdate, std::string> CompileUpdate(Compiler& compiler,
                                                         const SourceSession& source,
                                                         std::string text,
                                                         NativeCodeTier tier) {
  auto candidate = PrepareText(source, std::move(text));
  if (!candidate) {
    return std::unexpected(frontend::DiagnosticsText(candidate.error().diagnostics));
  }
  auto code = compiler.Compile(candidate->snapshot().program, {.tier = tier});
  if (!code) {
    return std::unexpected(frontend::DiagnosticsText(code.error()));
  }
  return CompiledUpdate{.source = std::move(*candidate), .code = std::move(*code)};
}

testing::AssertionResult PublishImage(runtime::Isolate& isolate,
                                      std::shared_ptr<const runtime::NativeImage> image,
                                      std::uint64_t revision) {
  auto& dispatch = isolate.native_dispatch();
  auto prepared = dispatch.Prepare(revision, std::move(image));
  if (!prepared) {
    return testing::AssertionFailure() << prepared.error().message;
  }
  const auto published = dispatch.Publish(std::move(*prepared));
  return published ? testing::AssertionSuccess()
                   : testing::AssertionFailure() << published.error().message;
}

testing::AssertionResult PublishSource(Compiler& compiler,
                                       SourceSession& source,
                                       runtime::Isolate& isolate,
                                       const SourceCandidate& candidate,
                                       NativeCodeTier tier) {
  const auto compiled = compiler.Compile(candidate.snapshot().program, {.tier = tier});
  if (!compiled) {
    return testing::AssertionFailure() << frontend::DiagnosticsText(compiled.error());
  }
  const auto published = PublishImage(isolate, compiled->image, source.snapshot()->revision);
  if (!published) {
    return published;
  }
  return source.Commit(candidate) ? testing::AssertionSuccess()
                                  : testing::AssertionFailure() << "source commit failed";
}

std::expected<void, std::string> UpdateSource(Compiler& compiler,
                                              SourceSession& source,
                                              runtime::Isolate& isolate,
                                              std::string text,
                                              NativeCodeTier tier,
                                              std::span<const DeclarationId> retained) {
  const auto candidate = PrepareText(source, std::move(text), retained);
  if (!candidate) {
    return std::unexpected(frontend::DiagnosticsText(candidate.error().diagnostics));
  }
  const auto published = PublishSource(compiler, source, isolate, *candidate, tier);
  if (!published) {
    return std::unexpected(published.message());
  }
  return {};
}

namespace {

testing::AssertionResult IsExpectedInteger(runtime::Value value, std::int64_t expected) {
  if (!value.IsInteger()) {
    return testing::AssertionFailure() << "result is not an integer";
  }
  const auto actual = value.AsInteger();
  return actual == expected
             ? testing::AssertionSuccess()
             : testing::AssertionFailure() << "expected " << expected << ", got " << actual;
}

}  // namespace

testing::AssertionResult CompletesWithInteger(runtime::Isolate& isolate,
                                              runtime::Value value,
                                              std::int64_t expected) {
  const runtime::RootedValue root(isolate, value);
  if (!root.value().IsObject()) {
    return testing::AssertionFailure() << "result is not a Future";
  }
  auto* future = runtime::Heap::GetFuture(*root.value().AsObject());
  if (future == nullptr) {
    return testing::AssertionFailure() << "result is not a Future";
  }
  if (!isolate.scheduler().PumpUntil(*future, runtime::FuturePumpMode::WaitForHost)) {
    return testing::AssertionFailure() << "Future did not complete";
  }
  const auto completed = future->value();
  return completed ? IsExpectedInteger(*completed, expected)
                   : testing::AssertionFailure() << "Future did not complete with a value";
}

testing::AssertionResult ReturnsInteger(const runtime::NativeNamedEntry& entry,
                                        std::int64_t expected,
                                        std::span<const runtime::Value> arguments) {
  const auto result = entry.Invoke(arguments);
  return result ? IsExpectedInteger(*result, expected)
                : testing::AssertionFailure() << ExecutionStatusName(result.error());
}

testing::AssertionResult RequiresReload(Compiler& compiler,
                                        runtime::Isolate& isolate,
                                        const SourceCandidate& candidate) {
  const auto code = compiler.Compile(candidate.snapshot().program);
  if (!code) {
    return testing::AssertionFailure() << frontend::DiagnosticsText(code.error());
  }
  auto& dispatch = isolate.native_dispatch();
  const auto prepared = dispatch.Prepare(dispatch.revision(), code->image);
  if (prepared) {
    return testing::AssertionFailure() << "incompatible image was accepted";
  }
  const auto& error = prepared.error();
  return error.status == runtime::NativePublicationStatus::RequiresReload &&
                 error.declaration != DeclarationId{} && !error.message.empty()
             ? testing::AssertionSuccess()
             : testing::AssertionFailure() << "missing reload diagnostic: " << error.message;
}

}  // namespace vela::compiler::jit::test
