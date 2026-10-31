#include "compiler/testing/suite.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/mock_operation.h"
#include "compiler/testing/result.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/member_table.h"
#include "runtime/native/native_instance.h"
#include "runtime/strings/string.h"
#include "runtime/testing/trace_value.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::testing {
namespace {

constexpr std::size_t kMockReportBudget = 16;
constexpr std::size_t kMockTextBudget = 4096;

std::string_view FailureName(MockFailureKind kind) {
  switch (kind) {
    case MockFailureKind::Usage:
      return "MockUsageError";
    case MockFailureKind::MissingStub:
      return "MockMissingStub";
    case MockFailureKind::StubExhausted:
      return "MockStubExhausted";
    case MockFailureKind::Verification:
      return "MockVerificationFailure";
    case MockFailureKind::Reset:
      return "MockReset";
    case MockFailureKind::SessionClosed:
      return "MockSessionClosed";
  }
  std::unreachable();
}

class MockReportReader final {
 public:
  explicit MockReportReader(const runtime::MemberTable& members) : layouts_(&members) {}
  static std::vector<runtime::Value> Items(runtime::Value value);
  [[nodiscard]] static std::vector<runtime::FutureTraceFrame> Trace(runtime::Value value);
  static std::vector<std::string> Strings(runtime::Value value);
  static std::string Text(runtime::Value value);
  static std::int64_t Number(runtime::Value value);
  [[nodiscard]] TestError::MockDetails Read(runtime::Value value) const;

 private:
  const runtime::MemberTable* layouts_;
};

std::vector<runtime::Value> MockReportReader::Items(runtime::Value value) {
  if (!value.IsObject()) {
    return {};
  }
  const auto count = runtime::Heap::ArrayLength(*value.AsObject());
  if (!count) {
    return {};
  }
  const auto size = std::min(static_cast<std::size_t>(*count), kMockReportBudget);
  std::vector<runtime::Value> items;
  items.reserve(size);
  for (std::size_t index = 0; index < size; ++index) {
    items.push_back(
        *runtime::Heap::GetElement(*value.AsObject(), static_cast<std::uint32_t>(index)));
  }
  return items;
}

std::string MockReportReader::Text(runtime::Value value) {
  if (!value.IsString()) {
    return "<invalid mock report text>";
  }
  const auto text = value.AsString()->value();
  // SDK 已对值显示施加预算；此处处理用户显式改写公开失败数据的边界。
  return text.size() <= kMockTextBudget ? std::string(text) : "<mock report text exceeds budget>";
}

std::int64_t MockReportReader::Number(runtime::Value value) {
  return value.IsInteger() ? value.AsInteger() : 0;
}

std::vector<std::string> MockReportReader::Strings(runtime::Value value) {
  std::vector<std::string> strings;
  for (const auto item : Items(value)) {
    strings.push_back(Text(item));
  }
  return strings;
}

std::vector<runtime::FutureTraceFrame> MockReportReader::Trace(runtime::Value value) {
  auto resolved = runtime::ReadTestTrace(value);
  if (!resolved) {
    return {};
  }
  return std::move(*resolved);
}

TestError::MockDetails MockReportReader::Read(runtime::Value value) const {
  const auto& layouts = *layouts_;
  TestError::MockDetails result{
      .session = Number(layouts.ReadPublicField(value, mock_fields::kSession)),
      .mock = Number(layouts.ReadPublicField(value, mock_fields::kMock)),
      .method = Text(layouts.ReadPublicField(value, mock_fields::kMethod)),
      .expected = Text(layouts.ReadPublicField(value, mock_fields::kExpected)),
      .actual_count = Number(layouts.ReadPublicField(value, mock_fields::kCount)),
      .calls = {},
      .configurations = Strings(layouts.ReadPublicField(value, mock_fields::kConfigurations)),
      .configuration_traces = {},
      .verification_trace = Trace(layouts.ReadPublicField(value, mock_fields::kVerificationTrace)),
  };
  for (const auto call : Items(layouts.ReadPublicField(value, mock_fields::kCalls))) {
    result.calls.push_back({
        .sequence = Number(layouts.ReadPublicField(call, mock_fields::kSequence)),
        .arguments = Strings(layouts.ReadPublicField(call, mock_fields::kArguments)),
        .omitted_arguments = Number(layouts.ReadPublicField(call, mock_fields::kOmittedArguments)),
        .trace = Trace(layouts.ReadPublicField(call, mock_fields::kTrace)),
    });
  }
  for (const auto trace :
       Items(layouts.ReadPublicField(value, mock_fields::kConfigurationTraces))) {
    result.configuration_traces.push_back(Trace(trace));
  }
  return result;
}

}  // namespace

void Suite::DescribeMockFailure(runtime::Value value, TestError& error) const {
  if (!value.IsObject()) {
    return;
  }
  const auto* layout = runtime::Heap::ClassOf(*value.AsObject());
  if (layout == nullptr) {
    return;
  }
  const auto found = mock_failures_.find(&layout->nominal_type());
  if (found == mock_failures_.end()) {
    return;
  }
  error.category = FailureName(found->second);
  const auto& layouts = instance_->members();
  const MockReportReader reader(layouts);
  const auto details = layouts.ReadPublicField(value, mock_fields::kDetails);
  if (!details.IsNull()) {
    error.mock = reader.Read(details);
  }
}

}  // namespace vela::testing
