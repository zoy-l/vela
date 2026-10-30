#include "compiler/testing/report.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <ostream>
#include <ratio>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <yyjson.h>

#include "base/json.h"
#include "compiler/testing/result.h"
#include "compiler/testing/settings.h"
#include "runtime/future.h"

namespace vela::testing {
namespace {

std::string_view EventName(ReportEventType type) {
  switch (type) {
    case ReportEventType::Start:
      return "start";
    case ReportEventType::Suite:
      return "suite";
    case ReportEventType::Group:
      return "group";
    case ReportEventType::Test:
      return "test";
    case ReportEventType::TestStart:
      return "testStart";
    case ReportEventType::Print:
      return "print";
    case ReportEventType::Warning:
      return "warning";
    case ReportEventType::Error:
      return "error";
    case ReportEventType::TestDone:
      return "testDone";
    case ReportEventType::Done:
      return "done";
  }
  std::unreachable();
}

std::string_view StatusName(TestStatus status) {
  switch (status) {
    case TestStatus::Pending:
      return "pending";
    case TestStatus::Passed:
      return "passed";
    case TestStatus::Failed:
      return "failed";
    case TestStatus::Skipped:
      return "skipped";
    case TestStatus::Cancelled:
      return "cancelled";
  }
  std::unreachable();
}

std::string_view PhaseName(TestPhase phase) {
  switch (phase) {
    case TestPhase::Registration:
      return "registration";
    case TestPhase::SetUpAll:
      return "setUpAll";
    case TestPhase::SetUp:
      return "setUp";
    case TestPhase::Body:
      return "body";
    case TestPhase::DynamicTearDown:
      return "addTearDown";
    case TestPhase::TearDown:
      return "tearDown";
    case TestPhase::TearDownAll:
      return "tearDownAll";
  }
  std::unreachable();
}

void StringField(yyjson_mut_doc* document,
                 yyjson_mut_val* object,
                 std::string_view key,
                 std::string_view text) {
  AddJsonField(document, object, key, yyjson_mut_strncpy(document, text.data(), text.size()));
}

yyjson_mut_val* JsonTrace(yyjson_mut_doc* document,
                          std::span<const runtime::FutureTraceFrame> trace) {
  auto* result = yyjson_mut_arr(document);
  for (const auto& frame : trace) {
    auto* item = yyjson_mut_obj(document);
    StringField(document, item, "function", frame.function);
    StringField(document, item, "file", frame.source_path);
    AddJsonField(document, item, "line", yyjson_mut_uint(document, frame.line));
    AddJsonField(document, item, "column", yyjson_mut_uint(document, frame.column));
    AddJsonField(document, item, "instruction", yyjson_mut_uint(document, frame.instruction));
    yyjson_mut_arr_add_val(result, item);
  }
  return result;
}

yyjson_mut_val* JsonDifference(yyjson_mut_doc* document, const Difference& difference) {
  auto* root = yyjson_mut_obj(document);
  std::vector<std::pair<const Difference*, yyjson_mut_val*>> pending{{&difference, root}};
  while (!pending.empty()) {
    const auto [source, object] = pending.back();
    pending.pop_back();
    StringField(document, object, "path", source->path);
    StringField(document, object, "actual", source->actual);
    auto* children = yyjson_mut_arr(document);
    for (const auto& child : source->children) {
      auto* item = yyjson_mut_obj(document);
      yyjson_mut_arr_add_val(children, item);
      pending.emplace_back(&child, item);
    }
    AddJsonField(document, object, "children", children);
  }
  return root;
}

const runtime::FutureTraceFrame* UserLocation(std::span<const runtime::FutureTraceFrame> trace) {
  for (const auto& frame : trace) {
    if (!frame.source_path.starts_with("sdk:") && !frame.source_path.starts_with("builtin:")) {
      return &frame;
    }
  }
  return trace.empty() ? nullptr : &trace.front();
}

void AddLocation(yyjson_mut_doc* document,
                 yyjson_mut_val* object,
                 std::span<const runtime::FutureTraceFrame> trace) {
  const auto* source = UserLocation(trace);
  if (source == nullptr) {
    return;
  }
  auto* location = yyjson_mut_obj(document);
  StringField(document, location, "file", source->source_path);
  AddJsonField(document, location, "line", yyjson_mut_uint(document, source->line));
  AddJsonField(document, location, "column", yyjson_mut_uint(document, source->column));
  AddJsonField(document, object, "location", location);
}

void ErrorFields(yyjson_mut_doc* document, yyjson_mut_val* object, const TestError& error) {
  StringField(document, object, "phase", PhaseName(error.phase));
  StringField(document, object, "category", error.category);
  StringField(document, object, "message", error.message);
  AddJsonField(document, object, "stack", JsonTrace(document, error.trace));
  AddLocation(document, object, error.trace);
  if (error.difference) {
    AddJsonField(document, object, "difference", JsonDifference(document, *error.difference));
  }
}

void TestFields(yyjson_mut_doc* document, yyjson_mut_val* object, const ReportEvent& event) {
  if (!event.name.empty()) {
    StringField(document, object, "name", event.name);
  }
  if (event.type == ReportEventType::Test || event.type == ReportEventType::Group) {
    auto* tags = yyjson_mut_arr(document);
    for (const auto& tag : event.tags) {
      yyjson_mut_arr_add_strncpy(document, tags, tag.data(), tag.size());
    }
    AddJsonField(document, object, "tags", tags);
    StringField(document, object, "skipReason", event.skip_reason);
    AddJsonField(document, object, "listed", yyjson_mut_bool(document, event.listed));
    AddLocation(document, object, event.trace);
    AddJsonField(document, object, "stack", JsonTrace(document, event.trace));
  }
  if (event.status) {
    StringField(document, object, "status", StatusName(*event.status));
  }
}

void WriteListedTest(std::ostream& output, const ReportEvent& event) {
  output << event.file << " :: " << event.name;
  if (!event.skip_reason.empty()) {
    output << " [skip: " << event.skip_reason << ']';
  }
  if (const auto* location = UserLocation(event.trace)) {
    output << " (" << location->source_path << ':' << location->line << ':' << location->column
           << ')';
  }
  for (const auto& tag : event.tags) {
    output << " #" << tag;
  }
  output << '\n';
}

void WriteFailure(std::ostream& output, const ReportEvent& event) {
  const auto& error = *event.error;
  output << event.file << " [" << PhaseName(error.phase) << "]: " << error.message << '\n';
  if (const auto* location = UserLocation(error.trace)) {
    output << "  at " << location->source_path << ':' << location->line << ':' << location->column
           << '\n';
  }
  if (error.difference) {
    output << "  " << error.difference->path << ": " << error.difference->actual << '\n';
  }
}

}  // namespace

SessionReporter::SessionReporter(Reporter kind, std::ostream& output, TerminalMode mode)
    : kind_(kind), output_(&output), started_(std::chrono::steady_clock::now()), mode_(mode) {}

void SessionReporter::Emit(const ReportEvent& event) {
  const std::scoped_lock lock(mutex_);
  const auto sequence = ++sequence_;
  const auto elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_);
  if (kind_ == Reporter::Json) {
    WriteJson(event, {.sequence = sequence, .milliseconds = elapsed.count()});
  } else {
    WriteHuman(event);
  }
  output_->flush();
  if (!output_->good()) {
    failed_ = true;
  }
}

void SessionReporter::WriteJson(const ReportEvent& event, Stamp stamp) {
  JsonOutput output;
  auto* document = output.document();
  auto* object = output.root();
  StringField(document, object, "type", EventName(event.type));
  StringField(document, object, "protocol", "vela.test");
  AddJsonField(document, object, "version", yyjson_mut_uint(document, 1));
  AddJsonField(document, object, "sequence", yyjson_mut_uint(document, stamp.sequence));
  AddJsonField(document, object, "time", yyjson_mut_real(document, stamp.milliseconds));
  if (event.suite != 0) {
    AddJsonField(document, object, "suiteID", yyjson_mut_uint(document, event.suite));
  }
  if (event.test) {
    AddJsonField(document, object, "testID", yyjson_mut_uint(document, *event.test));
  }
  if (event.scope) {
    AddJsonField(document, object, "scopeID", yyjson_mut_uint(document, *event.scope));
  }
  if (event.hook) {
    AddJsonField(document, object, "hookID", yyjson_mut_uint(document, *event.hook));
  }
  if (event.parent) {
    AddJsonField(document, object, "parentID", yyjson_mut_uint(document, *event.parent));
  }
  if (!event.file.empty()) {
    StringField(document, object, "file", event.file);
  }
  TestFields(document, object, event);
  if (event.type == ReportEventType::Print || event.type == ReportEventType::Warning) {
    StringField(document, object, "message", event.text);
    AddLocation(document, object, event.trace);
  }
  if (event.error) {
    ErrorFields(document, object, *event.error);
  }
  if (event.type == ReportEventType::Done) {
    AddJsonField(document, object, "success", yyjson_mut_bool(document, event.success));
    AddJsonField(document, object, "listed", yyjson_mut_bool(document, event.listed));
    AddJsonField(document, object, "selected", yyjson_mut_uint(document, event.selected));
    AddJsonField(document, object, "passed", yyjson_mut_uint(document, event.passed));
    AddJsonField(document, object, "failed", yyjson_mut_uint(document, event.failed));
    AddJsonField(document, object, "skipped", yyjson_mut_uint(document, event.skipped));
    AddJsonField(document, object, "cancelled", yyjson_mut_uint(document, event.cancelled));
  }
  const auto encoded = output.Serialize();
  if (!encoded) {
    failed_ = true;
    return;
  }
  *output_ << *encoded << '\n';
}

void SessionReporter::WriteHuman(const ReportEvent& event) {
  const bool progress = kind_ == Reporter::Compact && mode_ == TerminalMode::Interactive;
  if (progress_visible_ && event.type != ReportEventType::TestDone) {
    *output_ << "\r\033[K";
    progress_visible_ = false;
  }
  if (event.type == ReportEventType::Test && event.listed) {
    WriteListedTest(*output_, event);
  } else if (event.type == ReportEventType::TestDone) {
    ++completed_;
    if (progress) {
      *output_ << "\r\033[K[" << completed_ << "] ";
      progress_visible_ = true;
    }
    *output_ << event.file << " :: " << event.name << " [" << StatusName(*event.status) << ']';
    if (!event.skip_reason.empty()) {
      *output_ << ' ' << event.skip_reason;
    }
    if (!progress) {
      *output_ << '\n';
    }
  } else if (event.type == ReportEventType::Warning) {
    *output_ << event.file << ": warning: " << event.text << '\n';
  } else if (event.type == ReportEventType::Print) {
    *output_ << event.file << ": " << event.text << '\n';
  } else if (event.error) {
    WriteFailure(*output_, event);
  } else if (event.type == ReportEventType::Done) {
    if (event.listed) {
      *output_ << "Listed " << event.selected << " test(s)";
    } else {
      *output_ << event.passed << " passed, " << event.failed << " failed, " << event.skipped
               << " skipped";
    }
    if (event.cancelled != 0) {
      *output_ << ", " << event.cancelled << " cancelled";
    }
    *output_ << (event.success ? "\n" : " (unsuccessful)\n");
  }
}

}  // namespace vela::testing
