#include "compiler/frontend/builtins/sdk_modules.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "base/builtin.h"
#include "base/mock_operation.h"
#include "base/test_operation.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/sdk_sources.h"

namespace vela::frontend {
namespace {

struct SDKBinding final {
  std::string_view name;
  Builtin builtin;
  std::uint32_t operation;
};

constexpr SDKBinding BindOperation(std::string_view name, Builtin builtin, auto operation) {
  return {
      .name = name,
      .builtin = builtin,
      .operation = static_cast<std::uint32_t>(operation),
  };
}

constexpr auto kBindings = std::to_array<SDKBinding>({
    BindOperation("kindOfValue", Builtin::TestInspect, TestInspection::Kind),
    BindOperation("displayValue", Builtin::TestInspect, TestInspection::Display),
    BindOperation("trackExpectation", Builtin::TestControl, TestControl::TrackExpectation),
    BindOperation("registerCallback", Builtin::TestControl, TestControl::RegisterCallback),
    BindOperation("enterCallback", Builtin::TestControl, TestControl::EnterCallback),
    BindOperation("exitCallback", Builtin::TestControl, TestControl::ExitCallback),
    BindOperation("checkCallback", Builtin::TestControl, TestControl::CheckCallback),
    BindOperation("currentTestScope", Builtin::TestControl, TestControl::CurrentTestScope),
    BindOperation("registerFinalizer", Builtin::TestControl, TestControl::RegisterFinalizer),
    BindOperation("releaseFinalizer", Builtin::TestControl, TestControl::ReleaseFinalizer),
    BindOperation("trackCallWait", Builtin::TestControl, TestControl::TrackCallWait),
    BindOperation("isVoidSignature", Builtin::MockInspect, MockOperation::IsVoid),
    BindOperation("acceptsReturn", Builtin::MockInspect, MockOperation::AcceptsReturn),
    BindOperation("signatureArity", Builtin::MockInspect, MockOperation::Arity),
    BindOperation("parameterName", Builtin::MockInspect, MockOperation::ParameterName),
    BindOperation("acceptsAnswer", Builtin::MockInspect, MockOperation::AcceptsAnswer),
    BindOperation("acceptsVoidAnswer", Builtin::MockInspect, MockOperation::AcceptsVoidAnswer),
    BindOperation("isThrowable", Builtin::MockInspect, MockOperation::IsThrowable),
    BindOperation("captureTrace", Builtin::MockInspect, MockOperation::CaptureTrace),
    BindOperation("resolveTrace", Builtin::MockInspect, MockOperation::ResolveTrace),
    BindOperation("registerFailure", Builtin::MockInspect, MockOperation::RegisterFailure),
    BindOperation("observeMatch", Builtin::MockInspect, MockOperation::ObserveMatch),
    BindOperation("failObservation", Builtin::MockInspect, MockOperation::FailObservation),
    BindOperation("acceptsResponseSignature",
                  Builtin::MockInspect,
                  MockOperation::AcceptsResponseSignature),
});

}  // namespace

std::optional<std::string_view> SdkModuleSource(std::string_view identity) noexcept {
  if (identity == kTestModuleName) {
    return internal::kTestSource;
  }
  if (identity == kMockModuleName) {
    return internal::kMockSource;
  }
  return std::nullopt;
}

void BindSdkDeclarations(std::string_view identity, Program& program) {
  if (identity != kTestModuleName && identity != kMockModuleName) {
    return;
  }
  for (auto& function : program.functions) {
    const auto* const binding = std::ranges::find(kBindings, function.name, &SDKBinding::name);
    if (binding != kBindings.end()) {
      function.intrinsic = binding->builtin;
      function.intrinsic_operation = binding->operation;
      function.native_symbol.clear();
    } else if (function.name == "publishRegistration") {
      function.intrinsic = Builtin::TestRegister;
      function.native_symbol.clear();
    } else if (function.name == "sameValue") {
      function.intrinsic = Builtin::TestSame;
      function.native_symbol.clear();
    } else if (function.name == "test" || function.name == "group") {
      function.optional_parameters = {"skip", "timeout", "tags"};
    } else if (function.name == "expect" || function.name == "expectLater") {
      function.optional_parameters = {"reason"};
    } else if (function.name == "expectAsync0" || function.name == "expectAsync1") {
      function.optional_parameters = {"count", "max"};
    }
  }
}

}  // namespace vela::frontend
