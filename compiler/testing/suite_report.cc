#include "compiler/testing/suite.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "base/mock_operation.h"
#include "compiler/testing/report.h"
#include "compiler/testing/result.h"

namespace vela::testing {

void Suite::RegistrationError(std::string category, std::string message) {
  result_.errors.push_back({
      .phase = TestPhase::Registration,
      .category = std::move(category),
      .message = std::move(message),
      .trace = {},
      .difference = std::nullopt,
  });
  ReportEvent event(ReportEventType::Error);
  event.error = result_.errors.back();
  Emit(std::move(event));
}

void Suite::RegisterMockFailure(const runtime::NominalType& type, MockFailureKind kind) {
  mock_failures_.try_emplace(&type, kind);
}

void Suite::Emit(ReportEvent event) const {
  if (!options_.emit) {
    return;
  }
  event.suite = options_.suite;
  event.file = options_.file;
  options_.emit(event);
}

void Suite::EmitTest(const Node& node, ReportEventType type) const {
  const auto& result = result_.tests.at(*node.result);
  ReportEvent event(type);
  event.test = node.id;
  event.scope = node.id;
  event.parent = node.parent->id;
  event.name = result.name;
  event.tags = result.tags;
  event.skip_reason = result.skip_reason;
  event.trace = result.registration_trace;
  event.status = result.status;
  event.listed = options_.list;
  Emit(std::move(event));
}

std::optional<std::uint64_t> Suite::CurrentTestId() const {
  const auto context = isolate_->scheduler().context();
  if (!context) {
    return std::nullopt;
  }
  const auto* scope = scopes_.at(context->scope);
  return scope->kind == NodeKind::Test ? std::optional(scope->id) : std::nullopt;
}

SuiteResult Suite::Finish() {
  state_ = State::Finished;
  isolate_->scheduler().Shutdown();
  return std::move(result_);
}

}  // namespace vela::testing
