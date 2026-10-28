"""Link emitted native objects with the standalone runtime and execute GoogleTest."""

from __future__ import annotations

import os
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

from tools.config import ROOT, executable_name, static_library_name, toolchain_tools
from tools.aot_async_fixture import HOST_TESTS as ASYNC_COMPOSITION_TESTS, SOURCE as ASYNC_COMPOSITION_SOURCE
from tools.aot_api_fixture import HOST_TESTS as PUBLIC_API_TESTS, SOURCE as PUBLIC_API_SOURCE


HOST_TESTS = r'''
#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <gtest/gtest.h>
#include "base/execution_status.h"
#include "runtime/exception.h"
#include "runtime/future.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_call.h"
#include "runtime/native_image.h"
#include "runtime/native_instance.h"
#include "runtime/native_function.h"

using vela::ExecutionStatus;
using namespace vela::runtime;
using Entry = std::uint8_t (*)(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t INTEGER_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t FLOAT_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t BOOL_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t NULLABLE_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t NULL_TEST_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t REFERENCE_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t EXCEPTION_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t GLOBAL_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t NATIVE_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t FUTURE_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t LARGE_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t CALENDAR_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t JSON_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t CALLABLE_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t ASYNC_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t ASYNC_HOST_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" const NativeImage* INTEGER_IMAGE() noexcept;
extern "C" const NativeImage* FLOAT_IMAGE() noexcept;
extern "C" const NativeImage* BOOL_IMAGE() noexcept;
extern "C" const NativeImage* NULLABLE_IMAGE() noexcept;
extern "C" const NativeImage* NULL_TEST_IMAGE() noexcept;
extern "C" const NativeImage* REFERENCE_IMAGE() noexcept;
extern "C" const NativeImage* EXCEPTION_IMAGE() noexcept;
extern "C" const NativeImage* GLOBAL_IMAGE() noexcept;
extern "C" const NativeImage* NATIVE_IMAGE() noexcept;
extern "C" const NativeImage* FUTURE_IMAGE() noexcept;
extern "C" const NativeImage* LARGE_IMAGE() noexcept;
extern "C" const NativeImage* CALENDAR_IMAGE() noexcept;
extern "C" const NativeImage* JSON_IMAGE() noexcept;
extern "C" const NativeImage* CALLABLE_IMAGE() noexcept;
extern "C" const NativeImage* ASYNC_IMAGE() noexcept;
extern "C" const NativeImage* ASYNC_HOST_IMAGE() noexcept;

Entry ImageEntry(const NativeImage& image) {
  for (const auto& function : image.functions.values()) {
    if (function.name.text() == "calculate") { return function.entry; }
  }
  return nullptr;
}

NativeValue Integer(std::int64_t value) {
  return {.payload = std::bit_cast<std::uint64_t>(value), .kind = NativeValueKind::Integer};
}
NativeValue Double(double value) {
  return {.payload = std::bit_cast<std::uint64_t>(value), .kind = NativeValueKind::Double};
}
ExecutionStatus Execute(Entry entry, NativeExecution& execution,
                        std::span<const NativeValue> arguments, NativeValue& result) {
  return static_cast<ExecutionStatus>(entry(&execution, arguments.data(), arguments.size(), &result));
}

std::string CopyWithGc(NativeContext& context, std::string_view text) noexcept {
  context.isolate().heap().Collect();
  return std::string(text);
}
double Scale(NativeContext&, double value, bool enabled) noexcept { return enabled ? value * 2 : value; }
void Collect(NativeContext& context) noexcept { context.isolate().heap().Collect(); }
std::expected<void, NativeError> Fail(NativeContext&) noexcept {
  return std::unexpected(NativeError{.message = "host rejected request"});
}
std::string InvalidString(NativeContext&) noexcept { return std::string(1, static_cast<char>(0xff)); }
Future Ready(NativeContext&) noexcept { return Future::Value(Value::Integer(42)); }
Future Wrong(NativeContext&) noexcept { return Future::Value(Value::Boolean(true)); }
Future WrongError(NativeContext&) noexcept { return Future::Error(Value::Integer(42)); }
Completer*& ActiveProducer() { static Completer* producer = nullptr; return producer; }
Future Fetch(NativeContext& context) noexcept {
  auto& producer = *ActiveProducer();
  producer = context.CreateCompleter();
  return producer.future();
}

TEST(NativeHostTest, OptimizedIntegerCallsLoopsAndShiftSemantics) {
  Isolate isolate;
  const auto instance = NativeInstance::Instantiate(isolate, *INTEGER_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const std::array<std::array<std::int64_t, 4>, 7> cases{{
      {0, -3, -4, 12}, {0, 3, -4, -12}, {3, -1, 1, -2},
      {3, 1, -1, std::numeric_limits<std::int64_t>::min()},
      {4, -8, 65, -4}, {5, -1, 64, -1}, {6, 0, 0, 90},
  }};
  for (const auto& values : cases) {
    const std::array arguments{Integer(values[0]), Integer(values[1]), Integer(values[2])};
    NativeValue result;
    ASSERT_EQ(Execute(INTEGER_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(result.kind, NativeValueKind::Integer);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), values[3]);
    EXPECT_EQ(execution.frame_count(), 0U);
  }
}

TEST(NativeHostTest, ErrorsAndDepthLimitsCleanUpAllNativeFrames) {
  Isolate isolate;
  const auto instance = NativeInstance::Instantiate(isolate, *INTEGER_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  struct Case { std::int64_t operation; std::int64_t left; std::int64_t right; ExecutionStatus status; };
  const std::array cases{
      Case{0, maximum, 2, ExecutionStatus::IntegerOverflow},
      Case{1, minimum, -1, ExecutionStatus::IntegerOverflow},
      Case{2, minimum, -1, ExecutionStatus::IntegerOverflow},
      Case{1, 1, 0, ExecutionStatus::DivisionByZero},
      Case{7, 100, 0, ExecutionStatus::StackOverflow},
  };
  for (const auto& item : cases) {
    const std::array arguments{Integer(item.operation), Integer(item.left), Integer(item.right)};
    NativeValue result;
    EXPECT_EQ(Execute(INTEGER_ENTRY, execution, arguments, result), item.status);
    EXPECT_EQ(execution.frame_count(), 0U);
  }
}

TEST(NativeHostTest, FloatingValuesPreserveNanInfinityAndSignedZero) {
  Isolate isolate;
  const auto instance = NativeInstance::Instantiate(isolate, *FLOAT_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const std::array arguments{Double(-0.0), Double(1.0)};
  NativeValue result;
  ASSERT_EQ(Execute(FLOAT_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.kind, NativeValueKind::Double);
  EXPECT_EQ(result.payload, std::bit_cast<std::uint64_t>(-0.0));
  const std::array infinity{Double(std::numeric_limits<double>::infinity()), Double(1.0)};
  ASSERT_EQ(Execute(FLOAT_ENTRY, execution, infinity, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<double>(result.payload), std::numeric_limits<double>::infinity());
  const std::array zero{Double(1.0), Double(-0.0)};
  EXPECT_EQ(Execute(FLOAT_ENTRY, execution, zero, result), ExecutionStatus::DivisionByZero);
  const std::array nan{Double(std::numeric_limits<double>::quiet_NaN()), Double(1.0)};
  ASSERT_EQ(Execute(FLOAT_ENTRY, execution, nan, result), ExecutionStatus::Returned);
  const auto value = std::bit_cast<double>(result.payload);
  EXPECT_NE(value, value);
}

TEST(NativeHostTest, BooleanAndNullableResultsUseExplicitShapes) {
  Isolate isolate;
  const auto instance = NativeInstance::Instantiate(isolate, *BOOL_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const auto nullable = NativeInstance::Instantiate(isolate, *NULLABLE_IMAGE());
  ASSERT_TRUE(nullable.has_value());
  NativeExecution nullable_execution(**nullable);
  const auto null_test = NativeInstance::Instantiate(isolate, *NULL_TEST_IMAGE());
  ASSERT_TRUE(null_test.has_value());
  NativeExecution null_execution(**null_test);
  const std::array boolean{NativeValue{.payload = 0, .kind = NativeValueKind::Boolean}};
  NativeValue result;
  ASSERT_EQ(Execute(BOOL_ENTRY, execution, boolean, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.kind, NativeValueKind::Boolean);
  EXPECT_EQ(result.payload, 1U);
  const std::array null{NativeValue{}};
  ASSERT_EQ(Execute(NULLABLE_ENTRY, nullable_execution, null, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.kind, NativeValueKind::Null);
  const std::array integer{Integer(std::numeric_limits<std::int64_t>::min())};
  ASSERT_EQ(Execute(NULLABLE_ENTRY, nullable_execution, integer, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.payload, integer.front().payload);
  ASSERT_EQ(Execute(NULL_TEST_ENTRY, null_execution, null, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.payload, 1U);
  ASSERT_EQ(Execute(NULL_TEST_ENTRY, null_execution, integer, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.payload, 0U);
}

TEST(NativeHostTest, ExportBoundariesRejectWrongCountsAndKinds) {
  Isolate isolate;
  const auto instance = NativeInstance::Instantiate(isolate, *INTEGER_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  NativeValue result;
  EXPECT_EQ(Execute(INTEGER_ENTRY, execution, {}, result), ExecutionStatus::InvalidEntry);
  const std::array wrong{Double(1.0)};
  EXPECT_EQ(Execute(BOOL_ENTRY, execution, wrong, result), ExecutionStatus::TypeError);
  const std::array malformed{NativeValue{.payload = 2, .kind = NativeValueKind::Boolean}};
  EXPECT_EQ(Execute(BOOL_ENTRY, execution, malformed, result), ExecutionStatus::TypeError);
  EXPECT_EQ(execution.frame_count(), 0U);
}

TEST(NativeHostTest, ReferencesSurviveEveryAllocationAcrossCallsFieldsAndArrayGrowth) {
  Isolate isolate(1);
  const auto* image = REFERENCE_IMAGE();
  const auto instance = NativeInstance::Instantiate(isolate, *image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  NativeValue result;
  ASSERT_EQ(Execute(REFERENCE_ENTRY, execution, {}, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.kind, NativeValueKind::Integer);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 18);
  EXPECT_EQ(execution.frame_count(), 0U);
  const auto layouts = isolate.layout_counts();
  EXPECT_LT(isolate.heap().object_count(), 100U);
  const auto second = NativeInstance::Instantiate(isolate, *image);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(isolate.layout_counts(), layouts);
  ASSERT_NE(image->strings.size, 0U);
  EXPECT_NE((*instance)->constant(0).AsString(), (*second)->constant(0).AsString());
  auto mismatch = *image;
  mismatch.abi_version++;
  const auto rejected = NativeInstance::Instantiate(isolate, mismatch);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().status, NativeInstantiationStatus::AbiMismatch);
  EXPECT_EQ(isolate.layout_counts(), layouts);
}

TEST(NativeHostTest, TypedCatchesFinallyAndExceptionPayloadSurviveCollection) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *EXCEPTION_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  for (const auto operation : {0, 1, 2, 4, 5, 6}) {
    const std::array arguments{Integer(operation)};
    NativeValue result;
    ASSERT_EQ(Execute(EXCEPTION_ENTRY, execution, arguments, result), ExecutionStatus::Returned)
        << operation;
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42) << operation;
    EXPECT_EQ(execution.frame_count(), 0U);
    EXPECT_FALSE(execution.HasException());
  }
}

TEST(NativeHostTest, RethrowKeepsOriginalLogicalSourceStackAfterAllFramesExit) {
  Isolate isolate(1);
  const auto* image = EXCEPTION_IMAGE();
  const auto instance = NativeInstance::Instantiate(isolate, *image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const std::array arguments{Integer(3)};
  NativeValue result;
  ASSERT_EQ(Execute(EXCEPTION_ENTRY, execution, arguments, result), ExecutionStatus::DivisionByZero);
  EXPECT_EQ(execution.frame_count(), 0U);
  ASSERT_TRUE(execution.HasException());
  isolate.heap().Collect();
  const auto& exception = ExceptionData::From(*execution.pending_exception().AsObject());
  EXPECT_EQ(exception.status, ExecutionStatus::DivisionByZero);
  EXPECT_EQ(exception.message(), "division by zero");
  const auto trace = exception.trace();
  ASSERT_EQ(trace.size(), 3U);
  const std::array names{"fail", "original", "calculate"};
  for (std::size_t index = 0; index < trace.size(); ++index) {
    const auto position = trace[index];
    const auto& function = image->functions.values()[position.function];
    EXPECT_EQ(function.name.text(), names[index]);
    ASSERT_LT(position.instruction, function.locations.size);
    EXPECT_GT(function.locations.values()[position.instruction].line, 0U);
  }
  const std::array success{Integer(0)};
  ASSERT_EQ(Execute(EXCEPTION_ENTRY, execution, success, result), ExecutionStatus::Returned);
  EXPECT_FALSE(execution.HasException());
}

TEST(NativeHostTest, LazyGlobalsRetryFailuresAndKeepEachInstancesStateIndependent) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *GLOBAL_IMAGE());
  const auto second = NativeInstance::Instantiate(isolate, *GLOBAL_IMAGE());
  ASSERT_TRUE(instance.has_value());
  ASSERT_TRUE(second.has_value());
  NativeExecution execution(**instance);
  NativeExecution other(**second);
  const std::array count{Integer(1)};
  NativeValue result;
  ASSERT_EQ(Execute(GLOBAL_ENTRY, execution, count, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 0);
  for (const auto operation : {0, 2, 3, 3, 4}) {
    const std::array arguments{Integer(operation)};
    ASSERT_EQ(Execute(GLOBAL_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42) << operation;
    EXPECT_EQ(execution.frame_count(), 0U);
  }
  ASSERT_EQ(Execute(GLOBAL_ENTRY, execution, count, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 2);
  ASSERT_EQ(Execute(GLOBAL_ENTRY, other, count, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 0);
  const std::array assignment{Integer(5)};
  ASSERT_EQ(Execute(GLOBAL_ENTRY, other, assignment, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42);
  ASSERT_EQ(Execute(GLOBAL_ENTRY, other, count, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 0);
}

TEST(NativeHostTest, TypedImportsShareCodecsGcAndHostFailureMessages) {
  Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<CopyWithGc>("copy")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Scale>("scale")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Collect>("collect")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Fail>("fail")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<InvalidString>("invalid")));
  const auto instance = NativeInstance::Instantiate(isolate, *NATIVE_IMAGE());
  ASSERT_TRUE(instance.has_value()) << instance.error().message;
  NativeExecution execution(**instance);
  for (const auto operation : {0, 1, 2, 3, 4}) {
    const std::array arguments{Integer(operation)};
    NativeValue result;
    ASSERT_EQ(Execute(NATIVE_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42) << operation;
    EXPECT_EQ(execution.frame_count(), 0U);
    EXPECT_FALSE(execution.HasException());
  }
}

TEST(NativeHostTest, ImportedFuturePayloadAndErrorAreCheckedWithoutVmExecution) {
  Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Ready>("ready")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Wrong>("wrong")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<WrongError>("wrongError")));
  const auto instance = NativeInstance::Instantiate(isolate, *FUTURE_IMAGE());
  ASSERT_TRUE(instance.has_value()) << instance.error().message;
  NativeExecution execution(**instance);
  for (const auto operation : {0, 1, 2}) {
    const std::array arguments{Integer(operation)};
    NativeValue result;
    ASSERT_EQ(Execute(FUTURE_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    auto value = DecodeNativeValue(result);
    RootRange root(isolate.heap(), &value, 1);
    auto* future = Heap::GetFuture(*value.AsObject());
    ASSERT_NE(future, nullptr);
    ASSERT_TRUE(isolate.scheduler().PumpUntil(*future));
    if (operation == 0) {
      ASSERT_TRUE(future->value().has_value());
      EXPECT_EQ(future->value()->AsInteger(), 42);
    } else {
      ASSERT_TRUE(future->error().has_value());
      const auto diagnostic = future->error_diagnostic();
      ASSERT_TRUE(diagnostic.has_value());
      EXPECT_EQ(diagnostic->status, ExecutionStatus::TypeError);
      ASSERT_FALSE(diagnostic->stack_trace.empty());
    }
    EXPECT_EQ(execution.frame_count(), 0U);
  }
}

TEST(NativeHostTest, LargeTypedFramesUseRecoverableStorageAndReleaseItOnEveryExit) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *LARGE_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const std::array arguments{Integer(1)};
  NativeValue result;
  ASSERT_EQ(Execute(LARGE_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 205120);
  EXPECT_EQ(execution.frame_count(), 0U);
  const std::array overflow{Integer(std::numeric_limits<std::int64_t>::max())};
  EXPECT_EQ(Execute(LARGE_ENTRY, execution, overflow, result), ExecutionStatus::IntegerOverflow);
  EXPECT_EQ(execution.frame_count(), 0U);
}

TEST(NativeHostTest, DatesAndRegularExpressionsUseSharedRuntimeSemanticsUnderGcPressure) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *CALENDAR_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  for (const auto operation : {0, 1, 2, 3}) {
    const std::array arguments{Integer(operation)};
    NativeValue result;
    ASSERT_EQ(Execute(CALENDAR_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42) << operation;
    EXPECT_EQ(execution.frame_count(), 0U);
  }
}

TEST(NativeHostTest, JsonTraversesNativeToJsonMethodsAndPreservesFailuresAndCycles) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *JSON_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  for (const auto operation : {0, 1, 2, 3, 4, 5, 6, 7}) {
    const std::array arguments{Integer(operation)};
    NativeValue result;
    ASSERT_EQ(Execute(JSON_ENTRY, execution, arguments, result), ExecutionStatus::Returned)
        << operation;
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42) << operation;
    EXPECT_EQ(execution.frame_count(), 0U);
    EXPECT_FALSE(execution.HasException());
  }
}

TEST(NativeHostTest, ClosuresSharedMutationAndBoundInterfaceMethodsCallNativeTargets) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *CALLABLE_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  for (const auto operation : {0, 1, 2, 3, 4}) {
    const std::array arguments{Integer(operation)};
    NativeValue result;
    ASSERT_EQ(Execute(CALLABLE_ENTRY, execution, arguments, result), ExecutionStatus::Returned)
        << operation;
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42) << operation;
    EXPECT_EQ(execution.frame_count(), 0U);
  }
}

TEST(NativeHostTest, ClosuresRetainTheirOriginInstancesAndReleaseUnreachableProgramState) {
  Isolate isolate(1);
  const auto baseline = isolate.heap().object_count();
  {
    const auto* image = CALLABLE_IMAGE();
    const auto functions = image->functions.values();
    const auto factory = std::ranges::find(functions, "globalCounter", [](const auto& function) {
      return function.name.text();
    });
    const auto read = std::ranges::find(functions, "readGlobal", [](const auto& function) {
      return function.name.text();
    });
    const auto retain = std::ranges::find(functions, "retainCycle", [](const auto& function) {
      return function.name.text();
    });
    ASSERT_NE(factory, functions.end());
    ASSERT_NE(read, functions.end());
    ASSERT_NE(retain, functions.end());
    const auto other = NativeInstance::Instantiate(isolate, *image);
    ASSERT_TRUE(other.has_value());
    Value callable;
    RootRange root(isolate.heap(), &callable, 1);
    {
      const auto origin = NativeInstance::Instantiate(isolate, *image);
      ASSERT_TRUE(origin.has_value());
      NativeExecution execution(**origin);
      NativeValue result;
      ASSERT_EQ(retain->entry(&execution, nullptr, 0, &result),
                static_cast<std::uint8_t>(ExecutionStatus::Returned));
      ASSERT_EQ(factory->entry(&execution, nullptr, 0, &result),
                static_cast<std::uint8_t>(ExecutionStatus::Returned));
      callable = DecodeNativeValue(result);
    }
    isolate.heap().Collect();
    NativeExecution execution(**other);
    NativeValue function = EncodeNativeValue(callable);
    std::array<NativeValue, 1> arguments;
    NativeValue result;
    for (const auto expected : {41, 42}) {
      ASSERT_EQ(vela_native_v1_call_indirect(&execution, &function, arguments.data(), 0, &result),
                static_cast<std::uint8_t>(ExecutionStatus::Returned));
      EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), expected);
    }
    ASSERT_EQ(read->entry(&execution, nullptr, 0, &result),
              static_cast<std::uint8_t>(ExecutionStatus::Returned));
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 0);
    const auto foreign = NativeInstance::Instantiate(isolate, *INTEGER_IMAGE());
    ASSERT_TRUE(foreign.has_value());
    NativeExecution wrong(**foreign);
    EXPECT_EQ(vela_native_v1_call_indirect(&wrong, &function, arguments.data(), 0, &result),
              static_cast<std::uint8_t>(ExecutionStatus::TypeError));
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), baseline);
}

TEST(NativeHostTest, NativeAwaitResumesTypedLocalsExceptionsFinallyAndEnumPayloads) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *ASYNC_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  for (const auto operation : {0, 1, 2, 3, 4, 5, 6}) {
    const std::array arguments{Integer(operation)};
    NativeValue result;
    ASSERT_EQ(Execute(ASYNC_ENTRY, execution, arguments, result), ExecutionStatus::Returned)
        << operation;
    auto value = DecodeNativeValue(result);
    RootRange root(isolate.heap(), &value, 1);
    auto* future = Heap::GetFuture(*value.AsObject());
    ASSERT_NE(future, nullptr);
    if (operation == 0) {
      EXPECT_FALSE(future->IsCompleted());
      const auto functions = ASYNC_IMAGE()->functions.values();
      const auto read = std::ranges::find(functions, "readTrace", [](const auto& function) {
        return function.name.text();
      });
      ASSERT_NE(read, functions.end());
      NativeValue trace;
      ASSERT_EQ(Execute(read->entry, execution, {}, trace), ExecutionStatus::Returned);
      EXPECT_EQ(std::bit_cast<std::int64_t>(trace.payload), 1);
    }
    if (operation == 2 || operation == 3) { EXPECT_TRUE(future->IsCompleted()); }
    ASSERT_TRUE(isolate.scheduler().PumpUntil(*future));
    ASSERT_EQ(future->state(), FutureState::Succeeded) << operation;
    ASSERT_TRUE(future->value().has_value());
    EXPECT_EQ(future->value()->AsInteger(), 42) << operation;
    EXPECT_EQ(execution.frame_count(), 0U);
    EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
    EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0U);
  }
}

TEST(NativeHostTest, SuspendedNativeTasksRetainStateAfterExternalInstanceRelease) {
  Isolate isolate(1);
  const auto baseline = isolate.heap().object_count();
  {
    Value value;
    RootRange root(isolate.heap(), &value, 1);
    {
      const auto instance = NativeInstance::Instantiate(isolate, *ASYNC_IMAGE());
      ASSERT_TRUE(instance.has_value());
      NativeExecution execution(**instance);
      const std::array arguments{Integer(4)};
      NativeValue result;
      ASSERT_EQ(Execute(ASYNC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
      value = DecodeNativeValue(result);
    }
    EXPECT_GT(isolate.scheduler().suspended_frame_count(), 0U);
    isolate.heap().Collect();
    auto* future = Heap::GetFuture(*value.AsObject());
    ASSERT_TRUE(isolate.scheduler().PumpUntil(*future));
    ASSERT_TRUE(future->value().has_value());
    EXPECT_EQ(future->value()->AsInteger(), 42);
    EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
    EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0U);
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), baseline);
}

TEST(NativeHostTest, NativeAsyncErrorsKeepOriginalSourceAndAwaitBoundaries) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *ASYNC_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const std::array arguments{Integer(7)};
  NativeValue result;
  ASSERT_EQ(Execute(ASYNC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
  auto value = DecodeNativeValue(result);
  RootRange root(isolate.heap(), &value, 1);
  auto* future = Heap::GetFuture(*value.AsObject());
  ASSERT_TRUE(isolate.scheduler().PumpUntil(*future));
  ASSERT_EQ(future->state(), FutureState::Failed);
  const auto diagnostic = future->error_diagnostic();
  ASSERT_TRUE(diagnostic.has_value());
  EXPECT_EQ(diagnostic->status, ExecutionStatus::DivisionByZero);
  EXPECT_EQ(diagnostic->message, "division by zero");
  ASSERT_FALSE(diagnostic->stack_trace.empty());
  EXPECT_EQ(diagnostic->stack_trace.front().function, "fail");
  EXPECT_GT(diagnostic->stack_trace.front().line, 0U);
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
}

TEST(NativeHostTest, UnreachableNativeAwaitCyclesReleaseSnapshotsAndProgramState) {
  Isolate isolate(1);
  const auto baseline = isolate.heap().object_count();
  {
    const auto instance = NativeInstance::Instantiate(isolate, *ASYNC_IMAGE());
    ASSERT_TRUE(instance.has_value());
    NativeExecution execution(**instance);
    const std::array arguments{Integer(8)};
    NativeValue result;
    ASSERT_EQ(Execute(ASYNC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 1U);
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
  EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0U);
  EXPECT_EQ(isolate.heap().object_count(), baseline);
}

TEST(NativeHostTest, NativeAwaitSnapshotsRetainOnlyLiveDefinitions) {
  Isolate isolate(1);
  const auto instance = NativeInstance::Instantiate(isolate, *ASYNC_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const std::array arguments{Integer(9)};
  NativeValue result;
  ASSERT_EQ(Execute(ASYNC_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
  auto value = DecodeNativeValue(result);
  RootRange root(isolate.heap(), &value, 1);
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 1U);
  EXPECT_LT(isolate.scheduler().saved_state_bytes(), 1024U);
  isolate.heap().Collect();
  auto* future = Heap::GetFuture(*value.AsObject());
  ASSERT_TRUE(isolate.scheduler().PumpUntil(*future));
  ASSERT_TRUE(future->value().has_value());
  EXPECT_EQ(future->value()->AsInteger(), 42);
  EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0U);
}

TEST(NativeHostTest, NativeAwaitReceivesHostThreadCompletionAfterInstanceRelease) {
  Isolate isolate(1);
  Completer producer;
  ActiveProducer() = &producer;
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Fetch>("fetch")));
  Value value;
  RootRange root(isolate.heap(), &value, 1);
  {
    const auto instance = NativeInstance::Instantiate(isolate, *ASYNC_HOST_IMAGE());
    ASSERT_TRUE(instance.has_value());
    NativeExecution execution(**instance);
    NativeValue result;
    ASSERT_EQ(Execute(ASYNC_HOST_ENTRY, execution, {}, result), ExecutionStatus::Returned);
    value = DecodeNativeValue(result);
  }
  ActiveProducer() = nullptr;
  isolate.heap().Collect();
  auto* future = Heap::GetFuture(*value.AsObject());
  ASSERT_NE(future, nullptr);
  EXPECT_FALSE(future->IsCompleted());
  std::atomic<bool> completed = false;
  std::thread worker([&] { completed.store(producer.Complete(Value::Integer(37)).has_value()); });
  const auto pumped = isolate.scheduler().PumpUntil(*future, FuturePumpMode::WaitForHost);
  worker.join();
  ASSERT_TRUE(pumped);
  EXPECT_TRUE(completed.load());
  ASSERT_TRUE(future->value().has_value());
  EXPECT_EQ(future->value()->AsInteger(), 42);
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
  EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0U);
}
'''


class NativeObjectTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="vela-aot-原生 ")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.executable = Path(os.environ.get(
            "VELA_TEST_EXECUTABLE", ROOT / "out" / "debug" / executable_name("vela")))
        self.output = self.executable.parent
        self.tools = toolchain_tools()

    def command(self, arguments: list[str | Path], expected: int = 0) -> subprocess.CompletedProcess:
        result = subprocess.run(list(map(str, arguments)), capture_output=True,
                                text=True, encoding="utf-8", errors="replace",
                                cwd=self.directory, timeout=120, check=False)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def emit(self, name: str, source: str) -> tuple[Path, dict]:
        entry = self.directory / (name + ".vela")
        entry.write_text(source, encoding="utf-8", newline="\n")
        object_file = self.directory / (name + (".obj" if sys.platform == "win32" else ".o"))
        exports = ["calculate", *{
            "callable_entry": ["globalCounter", "readGlobal", "retainCycle"],
            "async_entry": ["readTrace"],
        }.get(name, [])]
        self.command([self.executable, "build", entry, "--emit=obj",
                      *["--export=" + value for value in exports], "-o", object_file])
        nm = self.tools["cxx"].parent / executable_name("llvm-nm")
        symbols = self.command([nm, "--defined-only", "--extern-only", object_file]).stdout
        images = re.findall(r"\b(vela_aot_[a-f0-9]+_image)\b", symbols)
        self.assertEqual(len(images), 1, symbols)
        self.assertNotRegex(symbols, r"\bvela_aot_[a-f0-9]+_entry_[0-9]+\b")
        manifest = json.loads(Path(str(object_file) + ".aot.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["format_version"], 1)
        self.assertEqual(manifest["image"], images[0])
        self.assertEqual({entry["name"] for entry in manifest["exports"]}, set(exports))
        self.assertIn(manifest["exports"][0]["symbol"], symbols)
        self.assertTrue(Path(str(object_file) + ".h").is_file())
        entry.unlink()
        return object_file, manifest

    def test_statically_linked_optimized_native_contracts_without_vm(self) -> None:
        fixtures = {
            "INTEGER_ENTRY": """
func twice(value: int): int { return value * 2; }
func touch(): void {}
func depth(value: int): int { if (value == 0) { return 0; } return depth(value - 1) + 1; }
export func calculate(operation: int, left: int, right: int): int {
  touch();
  if (operation == 0) { return left * right; }
  if (operation == 1) { return left / right; }
  if (operation == 2) { let remainder = left; remainder %= right; return remainder; }
  if (operation == 3) { return left << right; }
  if (operation == 4) { return left >> right; }
  if (operation == 5) { return left >>> right; }
  if (operation == 7) { return depth(left); }
  let sum = 0;
  for (let index = 0; index < 10; index++) { sum += twice(index); }
  return sum;
}
""",
            "FLOAT_ENTRY": "export func calculate(left: double, right: double): double { return left / right; }",
            "BOOL_ENTRY": "export func calculate(value: bool): bool { return !value; }",
            "NULLABLE_ENTRY": "export func calculate(value: int?): int? { return value; }",
            "NULL_TEST_ENTRY": "export func calculate(value: int?): bool { return value == null; }",
            "REFERENCE_ENTRY": """
class Box { param text: string; }
func make(value: int): string { return 'item:${value}'; }
func churn(): void {
  for (let index = 0; index < 1000; index++) { const garbage = Box(text: 'temporary'); }
}
export func calculate(): int {
  const text = make(42);
  const box = Box(text: text);
  const values = [text, box.text];
  churn();
  values.push('tail');
  box.text = 'changed';
  return values[0].length + values[1].length + values[2].length;
}
""",
            "EXCEPTION_ENTRY": """
class First {}
class Failure { param message: string; }
enum Choice { first, second }
func fail(): int { let zero = 0; return 1 / zero; }
func original(): int {
  try { return fail(); } finally { try { throw First(); } catch {} }
}
export func calculate(operation: int): int {
  if (operation == 0) {
    try { return fail(); }
    catch (error: First) { return 1; }
    catch (error: Error) {
      if (error.code == 'DivisionByZero' && error.message == 'division by zero') { return 42; }
      return 0;
    }
  }
  if (operation == 1) {
    let result = 42;
    try { return result; } finally { result = 1; }
  }
  if (operation == 2) {
    try {
      try { throw Failure(message: 'retained' + ' payload'); }
      finally {
        for (let index = 0; index < 200; index++) { const garbage = 'item:${index}'; }
      }
    } catch (error: Failure) {
      if (error.message == 'retained payload') { return 42; }
      return 0;
    }
  }
  if (operation == 3) { return original(); }
  if (operation == 4) {
    try { try { return 1; } finally { throw Failure(message: 'replacement'); } }
    catch (error: Failure) { return 42; }
  }
  if (operation == 5) {
    try { const value: int? = null; return value as int; }
    catch (error: Error) { if (error.code == 'InvalidCast') { return 42; } return 0; }
  }
  const boxed: dynamic = Choice.second;
  const value = boxed as Choice;
  if (value == Choice.second) { return 42; }
  return 0;
}
""",
            "GLOBAL_ENTRY": """
class Failure {}
let attempts = 0;
func initialize(): int {
  attempts += 1;
  if (attempts == 1) { throw Failure(); }
  return 42;
}
let answer = initialize();
let cycle = readCycle();
func readCycle(): int { return cycle; }
export func calculate(operation: int): int {
  if (operation == 0) {
    try { return answer; } catch (error: Failure) { return answer; }
  }
  if (operation == 1) { return attempts; }
  if (operation == 2) {
    const success: Expected<int, string> = ok(40);
    const failure: Expected<int, string> = err('no');
    const nested: Expected<Expected<int, string>, bool> = ok(ok(2));
    if (success.hasValue() && !failure.hasValue() && failure.error() == 'no') {
      return success.value() + nested.value().value();
    }
    return 0;
  }
  if (operation == 3) {
    try { return cycle; }
    catch (error: Error) { if (error.code == 'CyclicInitialization') { return 42; } return 0; }
  }
  if (operation == 4) {
    const failure: Expected<int, string> = err('inactive');
    try { return failure.value(); }
    catch (error: Error) { if (error.code == 'InactiveExpectedAlternative') { return 42; } return 0; }
  }
  answer = 42;
  return answer;
}
""",
            "NATIVE_ENTRY": """
external func copy(text: string): string;
external func scale(value: double, enabled: bool): double;
external func collect(): void;
external func fail(): void;
external func invalid(): string;
export func calculate(operation: int): int {
  if (operation == 0) {
    const text = copy('rooted' + ' argument');
    collect();
    if (text == 'rooted argument') { return 42; }
    return 0;
  }
  if (operation == 1) {
    if (scale(21.0, true) == 42.0 && scale(21.0, false) == 21.0) { return 42; }
    return 0;
  }
  if (operation == 2) {
    try { fail(); return 0; }
    catch (error: Error) {
      if (error.code == 'NativeFailure' && error.message == 'host rejected request') { return 42; }
      return 0;
    }
  }
  if (operation == 3) {
    try { invalid(); return 0; }
    catch (error: Error) {
      if (error.code == 'NativeFailure' && error.message == 'native function returned invalid UTF-8') { return 42; }
      return 0;
    }
  }
  let first = '';
  try { Math.sqrt(-1.0); }
  catch (error: Error) { first = error.message; }
  try { Random.nextInt(0); }
  catch (error: Error) {
    if (error.code == 'NativeFailure' && error.message != first) { return 42; }
  }
  return 0;
}
""",
            "FUTURE_ENTRY": """
external func ready(): Future<int>;
external func wrong(): Future<int>;
external func wrongError(): Future<int>;
export func calculate(operation: int): Future<int> {
  if (operation == 0) { return ready(); }
  if (operation == 1) { return wrong(); }
  return wrongError();
}
""",
            "CALENDAR_ENTRY": r"""
export func calculate(operation: int): int {
  if (operation == 0) {
    const date = Date.parse('1969-12-31T23:59:59.999Z');
    const parts = Date.toUTC(date);
    const restored = Date.fromUTC(parts.year, parts.month, parts.day, hour: parts.hour,
      minute: parts.minute, second: parts.second, millisecond: parts.millisecond);
    return date == -1 && restored == date && Date.toISOString(date) == '1969-12-31T23:59:59.999Z' ? 42 : 0;
  }
  if (operation == 1) {
    try { Date.parse('1900-02-29T00:00:00Z'); }
    catch (error: Error) { if (error.code == 'DateParseError') { return 42; } }
    return 0;
  }
  if (operation == 2) {
    const re = RegExp('(?<word>\\w+)-(\\d+)(x)?()');
    const found = re.firstMatch('😀语言-٤٢');
    if (found == null) { return 0; }
    const groups: string?[] = found.groups([0, 1, 2, 3, 4]);
    return found.start == 1 && found.end == 6 && found.groupCount == 4 &&
      found[1] == '语言' && found.namedGroup('word') == '语言' && groups[3] == null &&
      groups[4] == '' && found.groupNames[0] == 'word' ? 42 : 0;
  }
  try { RegExp('('); }
  catch (error: Error) { if (error.code == 'RegExpSyntaxError') { return 42; } }
  return 0;
}
""",
            "JSON_ENTRY": """
class Box { public toJson(): int { return 42; } }
class Failure {}
class Throwing { public toJson(): int { throw Failure(); } }
class Missing {}
enum Choice { first }
class EnumBox { public toJson(): Choice { return Choice.first; } }
class Text {
  public param text: string;
  public toJson(): string {
    const saved = this.text;
    for (let index = 0; index < 200; index++) { const garbage = 'item:${index}'; }
    return saved;
  }
}
export func calculate(operation: int): int {
  if (operation == 0) { return Json.parse('42') as int; }
  if (operation == 1) { return Json.stringify(Box()) == '42' ? 42 : 0; }
  if (operation == 2) {
    const source = '{"items":[1,true,null,"字"]}';
    return Json.stringify(Json.parse(source)) == source ? 42 : 0;
  }
  if (operation == 3) {
    try { Json.parse('{'); }
    catch (error: Error) {
      return error.code == 'JsonDecodeError' && error.message.includes(' at byte ') ? 42 : 0;
    }
    return 0;
  }
  if (operation == 4) {
    try { Json.stringify(Throwing()); }
    catch (error: Failure) { return 42; }
    return 0;
  }
  if (operation == 5) {
    const values: dynamic[] = [];
    values.push(values);
    try { Json.stringify(values); }
    catch (error: Error) { return error.code == 'JsonEncodeError' ? 42 : 0; }
    return 0;
  }
  if (operation == 6) {
    try { Json.stringify(EnumBox()); }
    catch (error: Error) { return error.code == 'JsonEncodeError' ? 42 : 0; }
    return 0;
  }
  return Json.stringify(Text(text: 'retained')) == '\"retained\"' ? 42 : 0;
}
""",
            "CALLABLE_ENTRY": """
let instanceValue = 0;
let saved: (() => int)? = null;
export func retainCycle(): void { saved = (): int => 42; }
export func globalCounter(): () => int {
  instanceValue = 40;
  return (): int => { instanceValue += 1; return instanceValue; };
}
export func readGlobal(): int { return instanceValue; }
func add(left: int, right: int): int { return left + right; }
func counter(start: int): () => int {
  let current = start;
  return (): int => { current += 1; return current; };
}
func reference(): () => int {
  const text = 'a' + 'b';
  return (): int => {
    for (let index = 0; index < 200; index++) { const garbage = 'item:${index}'; }
    return text.length + 40;
  };
}
interface Measurable { measure(): int; }
class Rectangle implements Measurable {
  public param width: int;
  public param height: int;
  public measure(): int { return this.width * this.height; }
}
class Constant implements Measurable { public measure(): int { return 2; } }
func area(value: Measurable): int { return value.measure(); }
export func calculate(operation: int): int {
  if (operation == 0) { const f = add; return f(20, 22); }
  if (operation == 1) { const f = counter(40); f(); return f(); }
  if (operation == 2) { const f = reference(); return f(); }
  if (operation == 3) {
    const values: Measurable[] = [Rectangle(width: 5, height: 8), Constant()];
    const bound = values[1].measure;
    return area(values[0]) + bound();
  }
  let value = 0;
  const one = (): int => { value += 1; return value; };
  const two = (): int => { value += 2; return value; };
  one(); two(); value += 38;
  return one();
}
""",
            "ASYNC_ENTRY": """
class Failure { public param message: string; }
enum Choice { first, second }
let trace = 0;
export func readTrace(): int { return trace; }
func fail(): int { let zero = 0; return 1 / zero; }
async func failed(): Future<int> { return fail(); }
async func boundary(): Future<int> {
  trace = 1;
  const value = await 40;
  trace = trace * 10 + 2;
  return value + 2;
}
async func recover(): Future<int> {
  const retained = 'retained';
  let value = 0;
  try { await failed(); }
  catch (error: Error) {
    const code = error.code;
    await 0;
    if (code == 'DivisionByZero' && retained == 'retained') { value = 40; }
  } finally { await sleep(0); value += 2; }
  return value;
}
async func immediate(): Future<int> { return 42; }
async func churn(): Future<int> {
  const retained = 'retained';
  let count = 0;
  for (let index = 0; index < 100; index++) {
    const garbage = 'item:${index}';
    count += await 1;
  }
  return retained.length + count - 66;
}
async func choice(): Future<Choice> { return Choice.second; }
async func enumPayload(): Future<int> {
  const selected = await choice();
  return selected == Choice.second ? 42 : 0;
}
async func explicitError(): Future<int> {
  try { await Future.error<int>(Failure(message: 'retained')); }
  catch (error: Failure) {
    await 0;
    return error.message == 'retained' ? 42 : 0;
  }
  return 0;
}
async func unhandled(): Future<int> { await 0; return await failed(); }
async func orphan(): Future<int> { await Completer<int>().future; return 42; }
export func calculate(operation: int): Future<int> {
  if (operation == 0) { return boundary(); }
  if (operation == 1) { return recover(); }
  if (operation == 2) { return immediate(); }
  if (operation == 3) { return Future.sync((): int => 42); }
  if (operation == 4) { return churn(); }
  if (operation == 5) { return enumPayload(); }
  if (operation == 6) { return explicitError(); }
  if (operation == 8) { return orphan(); }
  if (operation == 9) { return sparse(); }
  return unhandled();
}
""",
            "ASYNC_HOST_ENTRY": """
external func fetch(): Future<int>;
export async func calculate(): Future<int> {
  const retained = 'alive';
  const value = await fetch();
  return retained.length + value;
}
""",
        }
        large_locals = "\n".join(f"const item_{index} = value + {index};" for index in range(640))
        large_sum = "\n".join(f"total += item_{index};" for index in range(640))
        fixtures["LARGE_ENTRY"] = (
            "export func calculate(value: int): int {\n" + large_locals
            + "\nlet total = 0;\n" + large_sum + "\nreturn total;\n}")
        dead_locals = "\n".join(f"const dead_{index} = 'dead:' + {index}.toString();"
                                for index in range(150))
        fixtures["ASYNC_ENTRY"] += (
            "\nasync func sparse(): Future<int> { const retained = 'alive';\n"
            + dead_locals + "\nawait 0; return retained.length + 37;\n}")
        fixtures["COMPOSITION_ENTRY"] = ASYNC_COMPOSITION_SOURCE
        fixtures["PUBLIC_ENTRY"] = PUBLIC_API_SOURCE
        fixtures["VOID_ENTRY"] = "export func calculate(): void {}"
        host = HOST_TESTS + ASYNC_COMPOSITION_TESTS + PUBLIC_API_TESTS
        host = re.sub(r'^extern "C" std::uint8_t \w+_ENTRY\([^\n]+\);\n', "", host, flags=re.MULTILINE)
        objects = []
        for name, source in fixtures.items():
            object_file, manifest = self.emit(name.lower(), source)
            image_symbol = manifest["image"]
            objects.append(object_file)
            host = host.replace(name, "ImageEntry(*" + image_symbol + "())")
            host = host.replace(name.replace("_ENTRY", "_IMAGE"), image_symbol)
            host = host.replace(name.replace("_ENTRY", "_NAMESPACE"), image_symbol.removesuffix("_image"))
            wrapper = next(entry["symbol"] for entry in manifest["exports"] if entry["name"] == "calculate")
            host = host.replace(name.replace("_ENTRY", "_WRAPPER"), wrapper)
            host = '#include "' + object_file.name + '.h"\n' + host
        source_file = self.directory / "host.cc"
        source_file.write_text(host, encoding="utf-8", newline="\n")
        c_source = self.directory / "headers.c"
        c_source.write_text("\n".join('#include "' + path.name + '.h"' for path in objects)
                            + "\nint probe(void) { return VELA_AOT_RETURNED; }\n",
                            encoding="utf-8", newline="\n")
        executable = self.directory / executable_name("host")
        runtime = self.output / "obj/runtime" / static_library_name("vela_runtime")
        gtest = [self.output / "obj/third_party" / static_library_name(name)
                 for name in ("gtest_main", "gtest")]
        includes = [ROOT, ROOT / "third_party/googletest/googletest/include", ROOT / "third_party/abseil-cpp"]
        if sys.platform == "win32":
            self.command([self.tools["cxx"], "/nologo", "/TC", "/W4", "/WX", "/c", c_source,
                          f"/I{ROOT}", f"/Fo{self.directory / 'headers.obj'}"])
            flags = ["/nologo", "/MT", "/O2", "/clang:-std=c++23", "/D_ITERATOR_DEBUG_LEVEL=0",
                     "/D_HAS_EXCEPTIONS=0", "/c",
                     f"/Fo{self.directory / 'host.obj'}", *[f"/I{path}" for path in includes]]
            self.command([self.tools["cxx"], *flags, source_file])
            self.command([self.tools["cxx"], "/nologo", "/MT", "-fuse-ld=lld", f"/Fe{executable}",
                          self.directory / "host.obj", *objects, runtime, *gtest])
        else:
            self.command([self.tools["cxx"], "-x", "c", "-Werror", "-c", c_source,
                          f"-I{ROOT}", "-o", self.directory / "headers.o"])
            flags = ["-std=c++23", "-O2", "-stdlib=libc++", "-fno-exceptions", "-pthread",
                     "-o", str(executable), *[f"-I{path}" for path in includes]]
            if sys.platform == "darwin":
                flags.extend(["-isysroot", str(self.tools["sysroot"])])
            self.command([self.tools["cxx"], *flags, source_file, *objects, runtime, *gtest])
        executed = self.command([executable])
        self.assertIn("[  PASSED  ] 30 tests.", executed.stdout)
        nm = self.tools["cxx"].parent / executable_name("llvm-nm")
        symbols = self.command([nm, "--defined-only", "--demangle", runtime]).stdout
        self.assertNotIn("vela::vm::", symbols)
        self.assertNotIn("vela::compiler::", symbols)
        self.assertNotIn("llvm::", symbols)

    def test_failed_compilation_preserves_existing_output(self) -> None:
        entry = self.directory / "invalid.vela"
        entry.write_text("export func calculate(): int { return true; }", encoding="utf-8")
        output = self.directory / "existing.obj"
        output.write_bytes(b"existing valid artifact")
        self.command([self.executable, "build", entry, "--emit=obj", "--export=calculate", "-o", output], 65)
        self.assertEqual(output.read_bytes(), b"existing valid artifact")


if __name__ == "__main__":
    unittest.main()
