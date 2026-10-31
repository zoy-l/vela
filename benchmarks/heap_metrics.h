#pragma once

#include <cstddef>

namespace benchmark {
class State;
}
namespace vela::runtime {
class Heap;
}

namespace vela::benchmarks {

void CollectHeapBaseline(benchmark::State& state, runtime::Heap& heap);
// 先记录采样结束时的分配/提交量，再完成当前周期并收集，返回存活字节数。
std::size_t ReportHeap(benchmark::State& state, runtime::Heap& heap);

}  // namespace vela::benchmarks
