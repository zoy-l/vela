#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <luajit.h>
#include <lualib.h>
}
#include <luajit_build.h>
#include "benchmarks/heap_metrics.h"
#include "benchmarks/native_standard_workloads.h"
#include "benchmarks/native_workloads.h"
#include "benchmarks/source.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_execution.h"

namespace vela::benchmarks {
namespace {

constexpr std::string_view kInputModule = "workloads/input.vela";
constexpr std::string_view kSetupEntry = "benchmarkSetup";
constexpr std::string_view kWorkloadEntry = "benchmarkEntry";

struct Problem final {
  std::int64_t size;
  std::int64_t expected;
};

enum class ArrayCapacity : std::uint8_t { Growing, Reserved };
enum class LuaMode : std::uint8_t { Interpreter, Jit };

struct Workload final {
  std::string_view name;
  NativeWorkload native;
  std::array<Problem, 2> problems;
  ArrayCapacity capacity = ArrayCapacity::Growing;
};

constexpr auto kWorkloads = std::to_array<Workload>({
    {
        .name = "sieve",
        .native = Sieve,
        .problems = {{{.size = 10000, .expected = 1229}, {.size = 100000, .expected = 9592}}},
    },
    {
        .name = "mandelbrot",
        .native = Mandelbrot,
        .problems = {{{.size = 64, .expected = 99673}, {.size = 128, .expected = 399752}}},
    },
    {
        .name = "quicksort",
        .native = Quicksort,
        .problems =
            {
                {
                    {.size = 1024, .expected = 21409011239},
                    {.size = 8192, .expected = 1455713821118},
                },
            },
    },
    {
        .name = "binary_trees",
        .native = BinaryTrees,
        .problems = {{{.size = 6, .expected = 2159}, {.size = 9, .expected = 17391}}},
    },
    {
        .name = "particles",
        .native = Particles,
        .problems = {{{.size = 128, .expected = 8411000}, {.size = 1024, .expected = 74921000}}},
    },
    {
        .name = "word_count",
        .native = WordCount,
        .problems = {{{.size = 256, .expected = 1088}, {.size = 2048, .expected = 65600}}},
    },
    {
        .name = "closure_pipeline",
        .native = ClosurePipeline,
        .problems = {{{.size = 1024, .expected = 28986399}, {.size = 8192, .expected = 264132249}}},
    },
    {
        .name = "array_growth",
        .native = ArrayGrowth,
        .problems =
            {
                {
                    {.size = 1024, .expected = 1047552},
                    {.size = 65536, .expected = 4294901760},
                },
            },
    },
    {
        .name = "array_growth_reserved",
        .native = ArrayGrowthReserved,
        .problems =
            {
                {
                    {.size = 1024, .expected = 1047552},
                    {.size = 65536, .expected = 4294901760},
                },
            },
        .capacity = ArrayCapacity::Reserved,
    },
    {
        .name = "dynamic_values",
        .native = DynamicValues,
        .problems = {{{.size = 256, .expected = 11390}, {.size = 2048, .expected = 702123}}},
    },
    {
        .name = "dynamic_dispatch",
        .native = DynamicDispatch,
        .problems = {{{.size = 256, .expected = 2796160}, {.size = 2048, .expected = 1431655424}}},
    },
    {
        .name = "json_roundtrip",
        .native = JsonRoundtrip,
        .problems = {{{.size = 64, .expected = 5911}, {.size = 1024, .expected = 587179}}},
    },
    {
        .name = "byte_data_little",
        .native = ByteDataLittle,
        .problems = {{{.size = 128, .expected = 8255}, {.size = 2048, .expected = 2096383}}},
    },
    {
        .name = "byte_data_big",
        .native = ByteDataBig,
        .problems = {{{.size = 128, .expected = 8128}, {.size = 2048, .expected = 2096128}}},
    },
    {
        .name = "regexp_records",
        .native = RegExpRecords,
        .problems = {{{.size = 32, .expected = 6304}, {.size = 1024, .expected = 6296576}}},
    },
    {
        .name = "numeric_parse",
        .native = NumericParse,
        .problems = {{{.size = 256, .expected = 65280}, {.size = 2048, .expected = 4192256}}},
    },
    {
        .name = "ascii_case",
        .native = AsciiCase,
        .problems = {{{.size = 64, .expected = 438}, {.size = 1024, .expected = 8106}}},
    },
    {
        .name = "utc_roundtrip",
        .native = UtcRoundtrip,
        .problems = {{{.size = 64, .expected = 2023344}, {.size = 1024, .expected = 524854188}}},
    },
});

constexpr int kTraceLimit = 1000;
constexpr int kMinimumWarmup = 128;
constexpr int kMaximumWarmup = 1024;
constexpr int kWarmupBatch = 16;
constexpr int kStableBatches = 2;
constexpr std::size_t kKilobyte = 1024;
constexpr std::string_view kTraceInspector = R"(
local limit = ...
jit.opt.start("maxtrace=" .. limit)
local util = require("jit.util")
local function snapshot()
  local count, bytes = 0, 0
  for trace = 1, limit do
    if util.traceinfo(trace) ~= nil then
      count = count + 1
      local code = util.tracemc(trace)
      bytes = bytes + #code
    end
  end
  return count, bytes
end
jit.off(snapshot, true)
return snapshot
)";

using LuaState = std::unique_ptr<lua_State, decltype(&lua_close)>;

struct LuaSnapshot final {
  std::size_t traces = 0;
  std::size_t code_bytes = 0;
  bool operator==(const LuaSnapshot&) const = default;
};

std::string LuaError(lua_State* lua) {
  const auto* message = lua_tostring(lua, -1);
  return message != nullptr ? std::string(message)
                            : "Lua error object: " + std::string(luaL_typename(lua, -1));
}

class LuaProgram final {
 public:
  std::expected<void, std::string> Prepare(std::string_view source,
                                           const std::string& name,
                                           Problem problem,
                                           ArrayCapacity capacity,
                                           LuaMode mode) {
    if (lua_ == nullptr) {
      return std::unexpected("LuaJIT state allocation failed");
    }
    capacity_ = capacity == ArrayCapacity::Reserved ? static_cast<int>(problem.size) : 0;
    Setup setup{
        .program = this,
        .source = source,
        .name = name.c_str(),
        .size = static_cast<int>(problem.size),
        .mode = mode,
    };
    // 初始化可能通过 Lua 的错误机制退出；受保护的 C 回调内没有需要析构的 C++ 局部对象。
    if (lua_cpcall(lua_.get(), Initialize, &setup) != 0) {
      return std::unexpected(LuaError(lua_.get()));
    }
    return {};
  }

  std::expected<lua_Number, std::string> Run() {
    auto* lua = lua_.get();
    lua_rawgeti(lua, LUA_REGISTRYINDEX, run_);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, input_);
    lua_pushinteger(lua, capacity_);
    if (lua_pcall(lua, 2, 1, 0) != 0) {
      return std::unexpected(LuaError(lua));
    }
    if (lua_type(lua, -1) != LUA_TNUMBER) {
      return std::unexpected("Lua workload returned a non-number");
    }
    const auto result = lua_tonumber(lua, -1);
    lua_pop(lua, 1);
    return result;
  }

  std::expected<LuaSnapshot, std::string> Snapshot() {
    auto* lua = lua_.get();
    lua_rawgeti(lua, LUA_REGISTRYINDEX, snapshot_);
    if (lua_pcall(lua, 0, 2, 0) != 0) {
      return std::unexpected(LuaError(lua));
    }
    const LuaSnapshot result{
        .traces = static_cast<std::size_t>(lua_tonumber(lua, -2)),
        .code_bytes = static_cast<std::size_t>(lua_tonumber(lua, -1)),
    };
    lua_pop(lua, 2);
    return result;
  }

  void Collect() { lua_gc(lua_.get(), LUA_GCCOLLECT, 0); }
  [[nodiscard]] std::size_t heap_bytes() const {
    return (static_cast<std::size_t>(lua_gc(lua_.get(), LUA_GCCOUNT, 0)) * kKilobyte) +
           static_cast<std::size_t>(lua_gc(lua_.get(), LUA_GCCOUNTB, 0));
  }

 private:
  struct Setup final {
    LuaProgram* program;
    std::string_view source;
    const char* name;
    int size;
    LuaMode mode;
  };

  static int Initialize(lua_State* lua) {
    auto* setup = static_cast<Setup*>(lua_touserdata(lua, 1));
    auto& program = *setup->program;
    luaL_openlibs(lua);
    const auto mode = setup->mode == LuaMode::Jit ? LUAJIT_MODE_ON : LUAJIT_MODE_OFF;
    if (luaJIT_setmode(lua, 0, LUAJIT_MODE_ENGINE | mode) == 0) {
      lua_pushliteral(lua, "Cannot select requested LuaJIT engine mode");
      return lua_error(lua);
    }
    if (luaL_loadbuffer(lua, setup->source.data(), setup->source.size(), setup->name) != 0) {
      return lua_error(lua);
    }
    lua_call(lua, 0, 1);
    if (lua_type(lua, -1) != LUA_TFUNCTION) {
      lua_pushliteral(lua, "Lua workload must return its entry function");
      return lua_error(lua);
    }
    program.run_ = luaL_ref(lua, LUA_REGISTRYINDEX);
    lua_createtable(lua, setup->size, 0);
    for (int index = 0; index < setup->size; ++index) {
      lua_pushinteger(lua, index);
      lua_rawseti(lua, -2, index + 1);
    }
    program.input_ = luaL_ref(lua, LUA_REGISTRYINDEX);
    if (luaL_loadbuffer(lua, kTraceInspector.data(), kTraceInspector.size(), "trace inspector") !=
        0) {
      return lua_error(lua);
    }
    lua_pushinteger(lua, kTraceLimit);
    lua_call(lua, 1, 1);
    program.snapshot_ = luaL_ref(lua, LUA_REGISTRYINDEX);
    return 0;
  }

  LuaState lua_{luaL_newstate(), lua_close};
  int run_ = LUA_NOREF;
  int input_ = LUA_NOREF;
  int snapshot_ = LUA_NOREF;
  int capacity_ = 0;
};

struct Sources final {
  std::string path;
  std::string workload;
  std::string input;

  compiler::jit::SourceResult Compile(compiler::jit::Compiler& compiler) const {
    const auto modules = std::to_array<frontend::SourceInput>({
        {.path = path, .text = workload},
        {.path = kInputModule, .text = input},
    });
    return compiler::jit::CompileModules(compiler, modules, path);
  }
};

std::optional<Sources> LoadSources(benchmark::State& state,
                                   const Workload& workload,
                                   Problem problem) {
  const auto path = "workloads/" + std::string(workload.name) + ".vela";
  auto source = LoadBenchmarkSource(state, path);
  auto input = LoadBenchmarkSource(state, kInputModule);
  if (!source || !input) {
    return std::nullopt;
  }
  // 包装入口只提供负载规模；保留独立脚本中的 main，便于直接运行相同实现。
  source->append("\nfunc ")
      .append(kSetupEntry)
      .append("(): void { data.prepare(")
      .append(std::to_string(problem.size))
      .append("); }\nfunc ")
      .append(kWorkloadEntry)
      .append("(): int { return run(); }\n");
  return Sources{.path = path, .workload = std::move(*source), .input = std::move(*input)};
}

bool IsExpected(const runtime::ExecutionResult& result, Problem problem) {
  return result.Succeeded() && result.value.IsInteger() &&
         result.value.AsInteger() == problem.expected;
}

void Compile(benchmark::State& state, Workload workload, Problem problem) {
  const auto sources = LoadSources(state, workload, problem);
  if (!sources) {
    return;
  }
  auto compiler = compiler::jit::Compiler::Create();
  if (!compiler) {
    state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
    return;
  }
  for (auto iteration : state) {
    auto compiled = sources->Compile(**compiler);
    if (!compiled.Succeeded()) {
      state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
      return;
    }
    benchmark::DoNotOptimize(compiled);
  }
  state.counters["source_bytes"] =
      static_cast<double>(sources->workload.size() + sources->input.size());
}

void Jit(benchmark::State& state, Workload workload, Problem problem) {
  const auto sources = LoadSources(state, workload, problem);
  if (!sources) {
    return;
  }
  runtime::Isolate isolate;
  auto compiler = compiler::jit::Compiler::Create();
  if (!compiler) {
    state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
    return;
  }
  const auto compiled = sources->Compile(**compiler);
  if (!compiled.Succeeded()) {
    state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
    return;
  }
  const auto loaded = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  if (!loaded) {
    state.SkipWithError(loaded.error().message);
    return;
  }
  auto& instance = **loaded;
  const auto setup = runtime::ExecuteNativeFunction(instance, *instance.FindFunction(kSetupEntry));
  if (!setup.Succeeded()) {
    state.SkipWithError("Input preparation failed: " + setup.message);
    return;
  }
  const auto entry = *instance.FindFunction(kWorkloadEntry);
  const auto warm = runtime::ExecuteNativeFunction(instance, entry);
  if (!IsExpected(warm, problem)) {
    state.SkipWithError("Warm-up failed or checksum mismatched: " + warm.message);
    return;
  }
  auto& heap = isolate.heap();
  // 先结束初始化和 warm-up 的垃圾周期，之后让生产 GC 自行触发，不按样本强制清理。
  heap.FinishCollection();
  heap.Collect();
  const auto baseline_bytes = heap.allocated_bytes();
  state.counters["baseline_bytes"] = static_cast<double>(baseline_bytes);
  for (auto iteration : state) {
    auto result = runtime::ExecuteNativeFunction(instance, entry);
    benchmark::DoNotOptimize(result);
    if (!IsExpected(result, problem)) {
      state.SkipWithError("Execution failed or checksum mismatched: " + result.message);
      return;
    }
  }
  const auto retained = ReportHeap(state, heap);
  if (retained != baseline_bytes) {
    state.SkipWithError("Execution retained temporary heap values after collection");
    return;
  }
  state.counters["result"] = static_cast<double>(problem.expected);
}

std::optional<std::string> ReadLuaSource(benchmark::State& state, const Workload& workload) {
  const auto name = workload.capacity == ArrayCapacity::Reserved ? "array_growth" : workload.name;
  return LoadBenchmarkSource(state, "workloads/" + std::string(name) + ".lua");
}

std::expected<void, std::string> CheckLua(LuaProgram& program, Problem problem) {
  auto result = program.Run();
  if (!result) {
    return std::unexpected(result.error());
  }
  benchmark::DoNotOptimize(*result);
  if (*result != static_cast<lua_Number>(problem.expected)) {
    return std::unexpected("Lua checksum mismatch");
  }
  return {};
}

struct LuaWarmup final {
  LuaSnapshot snapshot;
  int calls = 0;
};

std::expected<LuaWarmup, std::string> WarmLua(LuaProgram& program, Problem problem, LuaMode mode) {
  LuaWarmup warm;
  int stable = 0;
  const auto minimum = mode == LuaMode::Jit ? kMinimumWarmup : 1;
  const auto batch = mode == LuaMode::Jit ? kWarmupBatch : 1;
  while (warm.calls < kMaximumWarmup &&
         (warm.calls < minimum || (mode == LuaMode::Jit && stable < kStableBatches))) {
    for (int index = 0; index < batch; ++index) {
      if (const auto result = CheckLua(program, problem); !result) {
        return std::unexpected(result.error());
      }
      ++warm.calls;
    }
    const auto snapshot = program.Snapshot();
    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }
    stable = *snapshot == warm.snapshot ? stable + 1 : 0;
    warm.snapshot = *snapshot;
  }
  if (mode == LuaMode::Jit && (stable < kStableBatches || warm.snapshot.traces == 0)) {
    return std::unexpected("LuaJIT warm-up did not produce stable compiled traces");
  }
  if (mode == LuaMode::Interpreter && warm.snapshot.traces != 0) {
    return std::unexpected("LuaJIT interpreter unexpectedly compiled traces");
  }
  return warm;
}

void Lua(benchmark::State& state, Workload workload, Problem problem, LuaMode mode) {
  const auto source = ReadLuaSource(state, workload);
  if (!source) {
    return;
  }
  LuaProgram program;
  if (const auto ready =
          program.Prepare(*source, std::string(workload.name), problem, workload.capacity, mode);
      !ready) {
    state.SkipWithError(ready.error());
    return;
  }
  const auto warm = WarmLua(program, problem, mode);
  if (!warm) {
    state.SkipWithError(warm.error());
    return;
  }
  program.Collect();
  state.counters["baseline_bytes"] = static_cast<double>(program.heap_bytes());
  state.counters["warmup_calls"] = warm->calls;
  state.counters["warm_traces"] = static_cast<double>(warm->snapshot.traces);
  state.counters["warm_jit_code_bytes"] = static_cast<double>(warm->snapshot.code_bytes);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    if (const auto result = CheckLua(program, problem); !result) {
      state.SkipWithError(result.error());
      return;
    }
  }
  const auto allocated = program.heap_bytes();
  const auto final = program.Snapshot();
  if (!final) {
    state.SkipWithError(final.error());
    return;
  }
  state.counters["traces"] = static_cast<double>(final->traces);
  state.counters["jit_code_bytes"] = static_cast<double>(final->code_bytes);
  state.counters["allocated_bytes"] = static_cast<double>(allocated);
  program.Collect();
  state.counters["retained_bytes"] = static_cast<double>(program.heap_bytes());
  state.counters["result"] = static_cast<double>(problem.expected);
}

void FirstLua(benchmark::State& state, Workload workload, Problem problem, LuaMode mode) {
  const auto source = ReadLuaSource(state, workload);
  if (!source) {
    return;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    state.PauseTiming();
    {
      LuaProgram program;
      if (const auto ready = program.Prepare(*source, std::string(workload.name), problem,
                                             workload.capacity, mode);
          !ready) {
        state.ResumeTiming();
        state.SkipWithError(ready.error());
        return;
      }
      state.ResumeTiming();
      const auto result = CheckLua(program, problem);
      state.PauseTiming();
      if (!result) {
        state.ResumeTiming();
        state.SkipWithError(result.error());
        return;
      }
    }
    state.ResumeTiming();
  }
}

void CompileLua(benchmark::State& state, Workload workload, Problem /*problem*/) {
  const auto source = ReadLuaSource(state, workload);
  if (!source) {
    return;
  }
  const auto name = std::string(workload.name);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const LuaState lua(luaL_newstate(), lua_close);
    if (lua == nullptr) {
      state.SkipWithError("LuaJIT state allocation failed");
      return;
    }
    if (luaL_loadbuffer(lua.get(), source->data(), source->size(), name.c_str()) != 0) {
      state.SkipWithError(LuaError(lua.get()));
      return;
    }
    benchmark::DoNotOptimize(lua.get());
  }
  state.counters["source_bytes"] = static_cast<double>(source->size());
}

void FirstJit(benchmark::State& state, Workload workload, Problem problem) {
  const auto sources = LoadSources(state, workload, problem);
  if (!sources) {
    return;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    state.PauseTiming();
    {
      runtime::Isolate isolate;
      auto compiler = compiler::jit::Compiler::Create();
      if (!compiler) {
        state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
        return;
      }
      const auto compiled = sources->Compile(**compiler);
      if (!compiled.Succeeded()) {
        state.ResumeTiming();
        state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
        return;
      }
      const auto loaded = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
      if (!loaded) {
        state.SkipWithError(loaded.error().message);
        return;
      }
      auto& instance = **loaded;
      const auto setup =
          runtime::ExecuteNativeFunction(instance, *instance.FindFunction(kSetupEntry));
      if (!setup.Succeeded()) {
        state.ResumeTiming();
        state.SkipWithError("Input preparation failed: " + setup.message);
        return;
      }
      const auto entry = *instance.FindFunction(kWorkloadEntry);
      state.ResumeTiming();
      auto result = runtime::ExecuteNativeFunction(instance, entry);
      benchmark::DoNotOptimize(result);
      state.PauseTiming();
      if (!IsExpected(result, problem)) {
        state.ResumeTiming();
        state.SkipWithError("First execution failed or checksum mismatched: " + result.message);
        return;
      }
    }
    state.ResumeTiming();
  }
}

void Cpp(benchmark::State& state, Workload workload, Problem problem) {
  std::vector<std::int64_t> input(static_cast<std::size_t>(problem.size));
  std::ranges::iota(input, std::int64_t{0});
  if (workload.native(input) != problem.expected) {
    state.SkipWithError("C++ warm-up checksum mismatched");
    return;
  }
  for (auto iteration : state) {
    // 原生内核独立编译；输入屏障和结果屏障阻止编译器跨迭代合并整个负载。
    benchmark::DoNotOptimize(input.data());
    auto result = workload.native(input);
    benchmark::DoNotOptimize(result);
    if (result != problem.expected) {
      state.SkipWithError("C++ checksum mismatched");
      return;
    }
  }
  state.counters["result"] = static_cast<double>(problem.expected);
}

int RegisterWorkloads() {
  benchmark::AddCustomContext("luajit_revision", VELA_LUAJIT_REVISION);
  benchmark::AddCustomContext("luajit_version", LUAJIT_VERSION);
  benchmark::AddCustomContext("luajit_arch", VELA_LUAJIT_ARCH);
  benchmark::AddCustomContext("luajit_dualnum", VELA_LUAJIT_DUALNUM);
  benchmark::AddCustomContext("luajit_gc64", VELA_LUAJIT_GC64);
  benchmark::AddCustomContext("luajit_build",
                              "upstream amalgamation; default allocator; no FFI workloads");
  benchmark::AddCustomContext("luajit_trace_limit", std::to_string(kTraceLimit));
  benchmark::AddCustomContext("luajit_minimum_warmup", std::to_string(kMinimumWarmup));
  benchmark::AddCustomContext("luajit_maximum_warmup", std::to_string(kMaximumWarmup));
  for (const auto& workload : kWorkloads) {
    for (const auto problem : workload.problems) {
      const auto suffix = std::string(workload.name) + "/" + std::to_string(problem.size);
      benchmark::RegisterBenchmark("compare/cpp/" + suffix, Cpp, workload, problem)->UseRealTime();
      benchmark::RegisterBenchmark("compare/jit/" + suffix, Jit, workload, problem)->UseRealTime();
      benchmark::RegisterBenchmark("compare/luajit/" + suffix, Lua, workload, problem, LuaMode::Jit)
          ->UseRealTime();
      benchmark::RegisterBenchmark("compare/luajit_interp/" + suffix, Lua, workload, problem,
                                   LuaMode::Interpreter)
          ->UseRealTime();
      benchmark::RegisterBenchmark("compile/representative/" + suffix, Compile, workload, problem)
          ->UseRealTime();
      benchmark::RegisterBenchmark("compile/luajit/" + suffix, CompileLua, workload, problem)
          ->UseRealTime();
      benchmark::RegisterBenchmark("first/jit/" + suffix, FirstJit, workload, problem)
          ->UseRealTime();
      benchmark::RegisterBenchmark("first/luajit/" + suffix, FirstLua, workload, problem,
                                   LuaMode::Jit)
          ->UseRealTime();
      benchmark::RegisterBenchmark("first/luajit_interp/" + suffix, FirstLua, workload, problem,
                                   LuaMode::Interpreter)
          ->UseRealTime();
    }
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = RegisterWorkloads();

}  // namespace
}  // namespace vela::benchmarks
