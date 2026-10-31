#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_image.h"
#include "runtime/value/value.h"

namespace vela::compiler::jit::test {

std::expected<SourceCandidate, SourceCandidateError> PrepareText(
    const SourceSession& source,
    std::string text,
    std::span<const DeclarationId> retained = {});
std::expected<CompiledImage, frontend::Diagnostics>
CompileText(Compiler& compiler, std::string text, NativeCodeTier tier = NativeCodeTier::Baseline);
struct CompiledUpdate final {
  SourceCandidate source;
  CompiledImage code;
};
std::expected<CompiledUpdate, std::string> CompileUpdate(Compiler& compiler,
                                                         const SourceSession& source,
                                                         std::string text,
                                                         NativeCodeTier tier);
testing::AssertionResult PublishImage(runtime::Isolate& isolate,
                                      std::shared_ptr<const runtime::NativeImage> image,
                                      std::uint64_t revision = 0);
testing::AssertionResult PublishSource(Compiler& compiler,
                                       SourceSession& source,
                                       runtime::Isolate& isolate,
                                       const SourceCandidate& candidate,
                                       NativeCodeTier tier = NativeCodeTier::Baseline);
std::expected<void, std::string> UpdateSource(Compiler& compiler,
                                              SourceSession& source,
                                              runtime::Isolate& isolate,
                                              std::string text,
                                              NativeCodeTier tier,
                                              std::span<const DeclarationId> retained = {});
testing::AssertionResult CompletesWithInteger(runtime::Isolate& isolate,
                                              runtime::Value value,
                                              std::int64_t expected);
testing::AssertionResult ReturnsInteger(const runtime::NativeNamedEntry& entry,
                                        std::int64_t expected,
                                        std::span<const runtime::Value> arguments = {});
testing::AssertionResult RequiresReload(Compiler& compiler,
                                        runtime::Isolate& isolate,
                                        const SourceCandidate& candidate);

}  // namespace vela::compiler::jit::test
