# Vela 与 C++ 基准：内存优化前后对照

本报告记录 2026-11-23 在 Apple M1 Pro / macOS arm64 / Release O3 上的实测。
优化前、后各 **615 项，每项 2 次**，共 **2460 个重复样本**。
全部预期用例、重复次数、结果校验和现有资源检查通过。完整测量的全部样本均用于聚合，不扣减入口或 control 耗时。

“优化前”保留完整反射包装、原 selector 编号及 64 KiB 对象页；“优化后”使用可达 selector 压紧、
未引用反射入口裁剪及 16 KiB 对象页。两侧保留相同的其他未提交 SDK 修改，并使用冻结的公共链接输入。
耗时倍率表示“Vela 耗时 ÷ 同组 C++ 耗时”。变化表示“优化后耗时 ÷ 优化前耗时 − 1”；正数代表记录的耗时增加。

[滚动聚合摘要](../../benchmarks/results/rolling_summary.json)的 `previous`、`latest` 分别对应这两个版本。

## 每个工作量的 C++ 倍率

每格按“优化前 → 优化后”列出耗时倍率，倍率为该路径耗时除以同组 C++ 耗时；C++ = 1。
普通工作负载与自由函数使用 C++ direct；类绑定及其自由函数使用 C++ pointer；
反射使用匹配策略的手写 C++ 对照；保存回调使用匹配的 C++ `std::function`。
这里列出全部有匹配 C++ 对照的工作量，未计算平均倍率。每项绝对耗时、变化与 CV 见后文完整表。

### 普通工作负载

| 工作量 | 规模 | AOT | Baseline JIT | Optimized JIT |
| --- | ---: | ---: | ---: | ---: |
| `arithmetic` | 1,000 | 1.011 → 1.021 | 2.409 → 2.410 | 2.405 → 2.423 |
| `arithmetic` | 4,096 | 1.023 → 1.001 | 2.423 → 2.364 | 2.391 → 2.342 |
| `arithmetic` | 65,536 | 1.003 → 1.235 | 2.359 → 2.388 | 2.362 → 2.363 |
| `arrays` | 1,000 | 0.369 → 0.367 | 1.250 → 1.243 | 1.270 → 1.524 |
| `asyncWork` | 64 | 1.197 → 0.970 | 1.698 → 1.706 | 1.771 → 1.718 |
| `calls` | 1,000 | 1.058 → 1.260 | 73.937 → 73.428 | 4.413 → 4.440 |
| `dictionary` | 500,000 | 0.617 → 0.529 | 1.187 → 0.840 | 0.947 → 0.888 |
| `expLoop` | 1,000,000 | 1.106 → 1.096 | 2.253 → 2.263 | 2.258 → 2.242 |
| `fibonacciLoop` | 6,511,134 | 1.428 → 1.423 | 1.010 → 1.002 | 1.005 → 1.002 |
| `fibonacciRecursive` | 31 | 0.546 → 0.552 | 12.821 → 12.865 | 3.764 → 2.955 |
| `fields` | 1,000 | 0.211 → 0.184 | 78.088 → 77.041 | 0.890 → 0.890 |
| `float2string` | 10,000 | 1.046 → 1.113 | 1.055 → 1.068 | 1.066 → 1.085 |
| `host` | 1,000 | 1.342 → 1.334 | 4.533 → 4.460 | 4.433 → 4.422 |
| `interop` | 1,000,000 | 0.830 → 0.812 | 4.335 → 4.530 | 4.370 → 4.251 |
| `mandelbrot` | 64 | 1.020 → 1.018 | 1.157 → 1.147 | 1.039 → 1.039 |
| `nbodies` | 33,000 | 1.127 → 1.103 | 5.030 → 5.056 | 1.571 → 1.572 |
| `particles` | 50,000 | 2.033 → 1.675 | 8.243 → 8.307 | 3.268 → 3.291 |
| `primes` | 14,000 | 1.023 → 2.070 | 1.023 → 1.029 | 1.006 → 1.008 |
| `queen` | 8 | 1.219 → 1.255 | 7.414 → 7.644 | 2.803 → 2.880 |
| `sha256` | 32 | 1.085 → 1.084 | 21.605 → 21.954 | 1.743 → 1.865 |
| `sort` | 100,000 | 0.451 → 0.443 | 7.748 → 7.679 | 1.789 → 1.762 |
| `spectralNorm` | 500 | 1.353 → 0.968 | 1.050 → 0.974 | 1.048 → 0.968 |
| `spectralNormInt` | 500 | 1.042 → 1.048 | 1.042 → 1.047 | 1.094 → 1.401 |
| `string2float` | 10,000 | 1.221 → 1.254 | 1.729 → 1.700 | 1.334 → 1.287 |
| `temporaryObjects` | 1,000 | 0.782 → 0.787 | 14.398 → 13.251 | 3.650 → 3.568 |
| `tree` | 20,000 | 0.813 → 0.849 | 4.747 → 4.629 | 2.128 → 2.349 |

### Vela → C++ 自由函数

| 工作量 | 每批次数 | AOT | Baseline JIT | Optimized JIT | 可重载 Optimized |
| --- | ---: | ---: | ---: | ---: | ---: |
| `boolean` | 64 | 0.567 → 0.564 | 0.907 → 0.902 | 0.911 → 0.900 | 1.830 → 1.835 |
| `boolean` | 4,096 | 0.464 → 0.469 | 0.803 → 0.798 | 0.801 → 0.802 | 1.738 → 1.762 |
| `double` | 64 | 0.332 → 0.348 | 1.228 → 1.311 | 1.235 → 1.312 | 1.253 → 1.752 |
| `double` | 4,096 | 0.272 → 0.277 | 1.279 → 1.253 | 1.274 → 1.253 | 1.266 → 1.250 |
| `empty` | 64 | 0.866 → 0.852 | 1.362 → 1.362 | 1.397 → 1.350 | 2.734 → 2.722 |
| `empty` | 4,096 | 0.467 → 0.487 | 0.793 → 0.797 | 0.789 → 0.798 | 1.707 → 1.729 |
| `integer` | 64 | 0.958 → 0.977 | 1.525 → 1.543 | 1.716 → 1.549 | 3.130 → 3.216 |
| `integer` | 4,096 | 0.562 → 0.537 | 0.914 → 0.926 | 0.923 → 0.935 | 2.007 → 2.694 |
| `string_borrow` | 64 | 1.523 → 1.336 | 4.663 → 4.730 | 4.669 → 4.680 | 5.544 → 5.642 |
| `string_borrow` | 4,096 | 1.194 → 1.170 | 4.866 → 4.795 | 4.860 → 4.787 | 5.592 → 5.543 |
| `string_borrow_4096` | 64 | 1.341 → 1.331 | 4.668 → 4.676 | 4.747 → 4.669 | 5.649 → 5.657 |
| `string_borrow_4096` | 4,096 | 1.171 → 1.156 | 4.994 → 4.738 | 4.818 → 5.073 | 5.638 → 5.597 |
| `string_borrow_96` | 64 | 1.344 → 1.327 | 4.782 → 4.702 | 4.729 → 4.718 | 5.642 → 5.610 |
| `string_borrow_96` | 4,096 | 1.161 → 1.191 | 4.787 → 4.856 | 6.389 → 4.816 | 5.681 → 5.612 |

### Vela → C++ 类绑定

| 工作量 | AOT | Baseline JIT | Optimized JIT | 可重载 Baseline | 可重载 Optimized |
| --- | ---: | ---: | ---: | ---: | ---: |
| `alternating` | 1.452 → 0.790 | 1.036 → 1.054 | 1.033 → 1.048 | 2.429 → 2.473 | 2.446 → 2.514 |
| `boolean` | 0.553 → 0.548 | 0.712 → 0.719 | 0.778 → 0.792 | 2.722 → 2.691 | 2.713 → 2.683 |
| `empty` | 0.509 → 0.527 | 0.775 → 0.811 | 0.776 → 0.781 | 2.738 → 2.669 | 2.702 → 2.662 |
| `floating` | 0.388 → 0.391 | 0.423 → 0.417 | 0.412 → 0.422 | 1.588 → 1.599 | 1.585 → 1.589 |
| `getter` | 0.823 → 0.846 | 1.145 → 1.145 | 1.147 → 1.152 | 4.501 → 4.288 | 4.394 → 4.305 |
| `integer` | 0.811 → 0.833 | 1.132 → 1.163 | 1.246 → 1.281 | 4.399 → 4.333 | 4.350 → 4.352 |
| `nullable` | 0.932 → 0.887 | 1.159 → 1.185 | 1.154 → 1.180 | 3.428 → 3.445 | 3.423 → 3.483 |
| `setter` | 0.503 → 0.490 | 0.775 → 0.771 | 0.774 → 0.768 | 2.688 → 2.602 | 2.686 → 2.606 |
| `static` | 0.527 → 0.631 | 0.847 → 1.017 | 0.838 → 1.017 | 2.739 → 3.329 | 2.729 → 3.316 |
| `void` | 0.503 → 0.514 | 0.968 → 0.792 | 0.793 → 0.787 | 2.724 → 2.665 | 2.670 → 2.668 |

### 同一 C++ 类方法的自由函数绑定

| 工作量 | AOT | Baseline JIT | Optimized JIT | 可重载 Baseline | 可重载 Optimized |
| --- | ---: | ---: | ---: | ---: | ---: |
| `alternating` | 0.775 → 0.795 | 1.042 → 1.045 | 1.031 → 1.056 | 2.457 → 2.489 | 2.497 → 2.642 |
| `boolean` | 0.562 → 0.552 | 0.791 → 0.795 | 0.708 → 0.724 | 3.628 → 2.663 | 2.681 → 2.651 |
| `empty` | 0.661 → 0.505 | 0.785 → 0.783 | 0.832 → 0.783 | 2.678 → 2.620 | 2.671 → 2.616 |
| `floating` | 0.388 → 0.395 | 0.416 → 0.417 | 0.414 → 0.418 | 1.568 → 1.574 | 1.573 → 1.572 |
| `getter` | 0.832 → 0.828 | 1.148 → 1.155 | 1.141 → 1.157 | 4.301 → 4.253 | 4.287 → 4.253 |
| `integer` | 0.812 → 0.826 | 1.144 → 1.166 | 1.252 → 1.286 | 4.333 → 4.268 | 4.263 → 4.284 |
| `nullable` | 0.929 → 0.897 | 1.155 → 1.186 | 1.159 → 1.178 | 3.396 → 3.451 | 3.525 → 3.441 |
| `setter` | 0.502 → 0.490 | 0.773 → 0.762 | 0.952 → 0.769 | 2.671 → 2.570 | 2.666 → 2.580 |
| `static` | 0.522 → 0.633 | 0.838 → 1.023 | 0.838 → 1.019 | 2.737 → 3.332 | 2.751 → 3.451 |
| `void` | 0.506 → 0.503 | 1.595 → 0.787 | 0.773 → 0.785 | 2.654 → 2.619 | 2.753 → 2.612 |

### 反射字段与方法

| 工作量 | AOT | Closed JIT | Baseline JIT | Optimized JIT |
| --- | ---: | ---: | ---: | ---: |
| `call/cached` | 0.569 → 0.546 | 5.881 → 5.773 | 13.395 → 13.190 | 9.332 → 9.269 |
| `call/direct` | 1.820 → 1.887 | 147.640 → 143.461 | 278.874 → 279.757 | 276.849 → 278.494 |
| `call/member_lookup` | 0.653 → 0.642 | 4.107 → 3.296 | 6.138 → 6.239 | 4.151 → 4.223 |
| `call/type_member_lookup` | 0.909 → 1.140 | 1.641 → 2.071 | 2.733 → 3.436 | 1.976 → 2.484 |
| `field/cached` | 0.517 → 0.508 | 3.067 → 3.104 | 4.503 → 4.524 | 5.492 → 4.504 |
| `field/direct` | 1.863 → 2.147 | 16.227 → 16.302 | 71.039 → 70.125 | 12.102 → 12.335 |
| `field/member_lookup` | 0.757 → 0.851 | 2.394 → 2.701 | 3.492 → 3.997 | 3.488 → 3.986 |
| `field/type_member_lookup` | 0.915 → 0.846 | 1.953 → 1.811 | 2.613 → 2.419 | 2.607 → 2.417 |

`cached` 复用描述符；`member_lookup` 每次查成员；`type_member_lookup` 每次查类型再查成员；
`direct` 为普通直接访问对照。两侧 direct 均允许编译器消除冗余访问。

### C++ → Vela 回调

| 工作量 | AOT | 可重载 Optimized |
| --- | ---: | ---: |
| `boolean` | 1.128 → 1.107 | 1.129 → 1.129 |
| `double` | 0.769 → 1.049 | 0.751 → 1.034 |
| `integer_capture` | 1.126 → 1.118 | 1.123 → 1.138 |
| `integer_max` | 1.142 → 1.115 | 1.097 → 1.100 |
| `integer_negative` | 1.117 → 1.119 | 1.087 → 1.091 |
| `persistent_callback` | 7.574 → 7.557 | 7.607 → 7.607 |
| `saved_callback` | 1.085 → 1.081 | 1.089 → 1.099 |

### C++ → Vela 标量入口

| 入口 | 优化前 / C++ | 优化后 / C++ |
| --- | ---: | ---: |
| AOT generated | 2.660 | 2.670 |
| AOT target | 2.831 | 2.848 |
| Baseline JIT | 3.658 | 3.684 |
| Optimized JIT | 3.669 | 3.674 |

对照为相同运行时输入的 C++ direct 标量函数。
## 计时与覆盖范围

| 组 | 用例数 / 版本 | 计时单位 |
| --- | ---: | --- |
| `jit_compare` | 104 | nanoseconds per complete workload invocation |
| `native_compare` | 112 | nanoseconds per call |
| `native_class_compare` | 210 | nanoseconds per call |
| `reflection_compare` | 105 | nanoseconds per logical operation |
| `interop` | 71 | nanoseconds per call, nanoseconds per logical operation |
| `entry` | 9 | nanoseconds per logical operation |
| `gc_report` | 4 | nanoseconds per collection cycle |

编译、输入准备、实例化及预热不计入稳态工作量；常规分配、自动 GC、调用验证和结果检查按现有基准实现计时。
反射的 C++ 对照使用固定 receiver 和单个位置 int 参数的手写注册表；Vela 还保留 GC、热重载和通用参数契约。
反射 direct 控制允许内联和冗余访问消除。没有匹配 C++ 用例的操作在倍率列记为 `—`。
GC 用例测量完整增量回收周期，构建对象图在计时之外，GC 没有 C++ 对照。
进程 RSS 不在本套件的测量范围内；对象分配和 committed 字节只作为基准计数保存。

## 每项耗时、倍率与波动

所有耗时均为 ns，并按基准记录的 batch 数折算到每次调用、逻辑操作或完整工作量。
CV 是两个重复平均耗时的样本标准差除以均值；范围和全部资源计数保存在 JSON。

| 用例 | 前 ns | 后 ns | 耗时变化 | 前 / C++ | 后 / C++ | CV 前 / 后 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `aot/cpp/arithmetic/1000/real_time` | 326.897 | 325.036 | -0.57% | 1.000× | 1.000× | 1.41% / 0.23% |
| `aot/cpp/arithmetic/4096/real_time` | 1,316.540 | 1,340.744 | +1.84% | 1.000× | 1.000× | 0.69% / 2.82% |
| `aot/cpp/arithmetic/65536/real_time` | 21,167.889 | 21,004.121 | -0.77% | 1.000× | 1.000× | 0.01% / 0.41% |
| `aot/cpp/arrays/1000/real_time` | 2,218.804 | 2,222.559 | +0.17% | 1.000× | 1.000× | 0.21% / 0.84% |
| `aot/cpp/asyncWork/64/real_time` | 22,263.940 | 22,124.525 | -0.63% | 1.000× | 1.000× | 0.95% / 0.05% |
| `aot/cpp/calls/1000/real_time` | 153.203 | 152.996 | -0.14% | 1.000× | 1.000× | 0.18% / 0.72% |
| `aot/cpp/dictionary/500000/real_time` | 33,949,821.356 | 38,218,555.500 | +12.57% | 1.000× | 1.000× | 2.45% / 3.93% |
| `aot/cpp/expLoop/1000000/real_time` | 2,435,390.217 | 2,429,070.944 | -0.26% | 1.000× | 1.000× | 0.19% / 0.34% |
| `aot/cpp/fibonacciLoop/6511134/real_time` | 2,090,956.944 | 2,109,476.836 | +0.89% | 1.000× | 1.000× | 0.45% / 0.18% |
| `aot/cpp/fibonacciRecursive/31/real_time` | 4,182,926.925 | 4,181,879.596 | -0.03% | 1.000× | 1.000× | 0.05% / 0.37% |
| `aot/cpp/fields/1000/real_time` | 28.427 | 28.349 | -0.28% | 1.000× | 1.000× | 0.44% / 0.83% |
| `aot/cpp/float2string/10000/real_time` | 1,329,888.356 | 1,321,073.703 | -0.66% | 1.000× | 1.000× | 0.12% / 0.34% |
| `aot/cpp/host/1000/real_time` | 967.526 | 969.607 | +0.22% | 1.000× | 1.000× | 0.35% / 1.09% |
| `aot/cpp/interop/1000000/real_time` | 971,587.488 | 992,435.423 | +2.15% | 1.000× | 1.000× | 0.44% / 4.23% |
| `aot/cpp/mandelbrot/64/real_time` | 370,929.530 | 366,570.759 | -1.18% | 1.000× | 1.000× | 0.82% / 2.76% |
| `aot/cpp/nbodies/33000/real_time` | 1,635,976.485 | 1,626,803.708 | -0.56% | 1.000× | 1.000× | 0.66% / 0.79% |
| `aot/cpp/particles/50000/real_time` | 562,511.653 | 559,490.206 | -0.54% | 1.000× | 1.000× | 0.47% / 1.18% |
| `aot/cpp/primes/14000/real_time` | 7,033,545.825 | 7,056,837.724 | +0.33% | 1.000× | 1.000× | 0.60% / 0.66% |
| `aot/cpp/queen/8/real_time` | 48,079.840 | 46,546.287 | -3.19% | 1.000× | 1.000× | 7.44% / 5.65% |
| `aot/cpp/sha256/32/real_time` | 162,220.383 | 161,021.071 | -0.74% | 1.000× | 1.000× | 0.16% / 0.83% |
| `aot/cpp/sort/100000/real_time` | 4,128,967.343 | 4,163,449.007 | +0.84% | 1.000× | 1.000× | 0.59% / 0.78% |
| `aot/cpp/spectralNorm/500/real_time` | 1,753,030.338 | 1,914,741.665 | +9.22% | 1.000× | 1.000× | 0.00% / 12.53% |
| `aot/cpp/spectralNormInt/500/real_time` | 1,755,166.013 | 1,746,490.556 | -0.49% | 1.000× | 1.000× | 0.38% / 0.43% |
| `aot/cpp/string2float/10000/real_time` | 863,999.552 | 871,330.485 | +0.85% | 1.000× | 1.000× | 0.59% / 3.17% |
| `aot/cpp/temporaryObjects/1000/real_time` | 5,313.562 | 5,521.389 | +3.91% | 1.000× | 1.000× | 0.70% / 5.03% |
| `aot/cpp/tree/20000/real_time` | 5,496,635.206 | 5,329,560.142 | -3.04% | 1.000× | 1.000× | 0.27% / 0.57% |
| `aot/native/arithmetic/1000/real_time` | 330.482 | 331.878 | +0.42% | 1.011× | 1.021× | 0.10% / 0.04% |
| `aot/native/arithmetic/4096/real_time` | 1,346.939 | 1,341.465 | -0.41% | 1.023× | 1.001× | 0.06% / 0.87% |
| `aot/native/arithmetic/65536/real_time` | 21,233.868 | 25,945.217 | +22.19% | 1.003× | 1.235× | 0.32% / 26.45% |
| `aot/native/arrays/1000/real_time` | 818.592 | 815.448 | -0.38% | 0.369× | 0.367× | 0.08% / 0.78% |
| `aot/native/asyncWork/64/real_time` | 26,646.751 | 21,458.191 | -19.47% | 1.197× | 0.970× | 27.01% / 1.11% |
| `aot/native/calls/1000/real_time` | 162.090 | 192.747 | +18.91% | 1.058× | 1.260× | 1.51% / 23.73% |
| `aot/native/dictionary/500000/real_time` | 20,958,317.751 | 20,236,474.345 | -3.44% | 0.617× | 0.529× | 0.58% / 1.50% |
| `aot/native/expLoop/1000000/real_time` | 2,694,185.868 | 2,663,208.333 | -1.15% | 1.106× | 1.096× | 0.41% / 0.39% |
| `aot/native/fibonacciLoop/6511134/real_time` | 2,984,932.124 | 3,002,299.214 | +0.58% | 1.428× | 1.423× | 0.11% / 0.51% |
| `aot/native/fibonacciRecursive/31/real_time` | 2,282,645.835 | 2,308,344.967 | +1.13% | 0.546× | 0.552× | 0.07% / 1.40% |
| `aot/native/fields/1000/real_time` | 6.012 | 5.211 | -13.32% | 0.211× | 0.184× | 6.00% / 1.70% |
| `aot/native/float2string/10000/real_time` | 1,391,312.710 | 1,470,415.079 | +5.69% | 1.046× | 1.113× | 0.80% / 6.91% |
| `aot/native/host/1000/real_time` | 1,297.990 | 1,292.974 | -0.39% | 1.342× | 1.334× | 0.21% / 0.62% |
| `aot/native/interop/1000000/real_time` | 805,986.731 | 806,352.667 | +0.05% | 0.830× | 0.812× | 0.09% / 0.09% |
| `aot/native/mandelbrot/64/real_time` | 378,356.993 | 373,175.552 | -1.37% | 1.020× | 1.018× | 0.52% / 0.34% |
| `aot/native/nbodies/33000/real_time` | 1,843,640.622 | 1,793,692.223 | -2.71% | 1.127× | 1.103× | 0.30% / 2.57% |
| `aot/native/particles/50000/real_time` | 1,143,600.041 | 937,119.843 | -18.06% | 2.033× | 1.675× | 24.49% / 3.19% |
| `aot/native/primes/14000/real_time` | 7,193,523.214 | 14,604,902.150 | +103.03% | 1.023× | 2.070× | 2.27% / 72.74% |
| `aot/native/queen/8/real_time` | 58,609.240 | 58,396.478 | -0.36% | 1.219× | 1.255× | 3.68% / 0.62% |
| `aot/native/sha256/32/real_time` | 176,011.132 | 174,486.176 | -0.87% | 1.085× | 1.084× | 1.18% / 1.41% |
| `aot/native/sort/100000/real_time` | 1,860,833.887 | 1,844,897.626 | -0.86% | 0.451× | 0.443× | 1.34% / 0.20% |
| `aot/native/spectralNorm/500/real_time` | 2,371,029.106 | 1,853,257.082 | -21.84% | 1.353× | 0.968× | 32.00% / 1.20% |
| `aot/native/spectralNormInt/500/real_time` | 1,829,216.694 | 1,829,866.250 | +0.04% | 1.042× | 1.048× | 0.30% / 0.70% |
| `aot/native/string2float/10000/real_time` | 1,055,108.381 | 1,092,783.992 | +3.57% | 1.221× | 1.254× | 3.46% / 9.11% |
| `aot/native/temporaryObjects/1000/real_time` | 4,153.770 | 4,346.304 | +4.64% | 0.782× | 0.787× | 0.37% / 6.62% |
| `aot/native/tree/20000/real_time` | 4,470,273.476 | 4,524,579.690 | +1.21% | 0.813× | 0.849× | 1.42% / 2.36% |
| `entry/baseline_scalar/real_time` | 3.566 | 3.546 | -0.54% | 3.658× | 3.684× | 1.14% / 0.33% |
| `entry/cpp_direct/scalar/real_time` | 0.975 | 0.963 | -1.25% | 1.000× | 1.000× | 1.47% / 0.04% |
| `entry/cpp_indirect/scalar/real_time` | 0.980 | 0.965 | -1.56% | 1.006× | 1.002× | 0.27% / 0.04% |
| `entry/execution/real_time` | 6.365 | 6.378 | +0.20% | — | — | 0.05% / 0.24% |
| `entry/generated/scalar/real_time` | 2.593 | 2.570 | -0.89% | 2.660× | 2.670× | 0.30% / 0.18% |
| `entry/instance_borrow/real_time` | 20.402 | 20.362 | -0.20% | — | — | 0.32% / 0.14% |
| `entry/optimized_scalar/real_time` | 3.577 | 3.537 | -1.12% | 3.669× | 3.674× | 0.52% / 0.06% |
| `entry/stack_bounds/real_time` | 0.650 | 0.644 | -0.85% | — | — | 0.22% / 0.42% |
| `entry/target/scalar/real_time` | 2.760 | 2.742 | -0.66% | 2.831× | 2.848× | 1.18% / 0.44% |
| `gc/cohort/0/64/16/manual_time` | 24,484.366 | 25,024.661 | +2.21% | — | — | 0.48% / 0.09% |
| `gc/cohort/8192/4096/16/manual_time` | 116,992.268 | 117,689.437 | +0.60% | — | — | 0.39% / 0.61% |
| `gc/cohort/8192/64/16/manual_time` | 239,970.254 | 120,461.012 | -49.80% | — | — | 70.38% / 0.04% |
| `gc/cohort/8192/64/64/manual_time` | 99,753.883 | 100,442.560 | +0.69% | — | — | 0.40% / 0.21% |
| `hot/steady/baseline/arithmetic/1000/real_time` | 787.352 | 783.486 | -0.49% | 2.409× | 2.410× | 0.10% / 0.41% |
| `hot/steady/baseline/arithmetic/4096/real_time` | 3,190.078 | 3,169.280 | -0.65% | 2.423× | 2.364× | 1.57% / 0.46% |
| `hot/steady/baseline/arithmetic/65536/real_time` | 49,942.783 | 50,161.329 | +0.44% | 2.359× | 2.388× | 0.20% / 0.86% |
| `hot/steady/baseline/arrays/1000/real_time` | 2,774.488 | 2,762.724 | -0.42% | 1.250× | 1.243× | 0.51% / 0.16% |
| `hot/steady/baseline/asyncWork/64/real_time` | 37,810.235 | 37,734.916 | -0.20% | 1.698× | 1.706× | 0.16% / 0.43% |
| `hot/steady/baseline/calls/1000/real_time` | 11,327.320 | 11,234.209 | -0.82% | 73.937× | 73.428× | 0.02% / 0.38% |
| `hot/steady/baseline/dictionary/500000/real_time` | 40,283,368.056 | 32,119,852.100 | -20.27% | 1.187× | 0.840× | 29.63% / 8.81% |
| `hot/steady/baseline/expLoop/1000000/real_time` | 5,486,595.186 | 5,496,621.250 | +0.18% | 2.253× | 2.263× | 0.04% / 1.46% |
| `hot/steady/baseline/fibonacciLoop/6511134/real_time` | 2,112,668.545 | 2,113,156.087 | +0.02% | 1.010× | 1.002× | 0.09% / 0.21% |
| `hot/steady/baseline/fibonacciRecursive/31/real_time` | 53,629,729.099 | 53,800,062.402 | +0.32% | 12.821× | 12.865× | 1.51% / 4.25% |
| `hot/steady/baseline/fields/1000/real_time` | 2,219.834 | 2,184.013 | -1.61% | 78.088× | 77.041× | 1.10% / 1.52% |
| `hot/steady/baseline/float2string/10000/real_time` | 1,402,648.542 | 1,411,539.812 | +0.63% | 1.055× | 1.068× | 0.99% / 2.71% |
| `hot/steady/baseline/host/1000/real_time` | 4,385.664 | 4,324.509 | -1.39% | 4.533× | 4.460× | 0.92% / 1.83% |
| `hot/steady/baseline/interop/1000000/real_time` | 4,212,228.545 | 4,495,615.589 | +6.73% | 4.335× | 4.530× | 0.29% / 7.70% |
| `hot/steady/baseline/mandelbrot/64/real_time` | 429,010.795 | 420,546.072 | -1.97% | 1.157× | 1.147× | 2.06% / 0.99% |
| `hot/steady/baseline/nbodies/33000/real_time` | 8,229,300.853 | 8,225,188.103 | -0.05% | 5.030× | 5.056× | 0.57% / 0.05% |
| `hot/steady/baseline/particles/50000/real_time` | 4,637,053.270 | 4,647,749.652 | +0.23% | 8.243× | 8.307× | 0.79% / 0.33% |
| `hot/steady/baseline/primes/14000/real_time` | 7,198,468.487 | 7,259,158.115 | +0.84% | 1.023× | 1.029× | 0.38% / 1.23% |
| `hot/steady/baseline/queen/8/real_time` | 356,482.295 | 355,798.693 | -0.19% | 7.414× | 7.644× | 0.51% / 1.86% |
| `hot/steady/baseline/sha256/32/real_time` | 3,504,702.862 | 3,535,100.781 | +0.87% | 21.605× | 21.954× | 0.89% / 0.29% |
| `hot/steady/baseline/sort/100000/real_time` | 31,989,593.750 | 31,972,765.624 | -0.05% | 7.748× | 7.679× | 0.28% / 0.40% |
| `hot/steady/baseline/spectralNorm/500/real_time` | 1,840,682.461 | 1,864,115.585 | +1.27% | 1.050× | 0.974× | 0.35% / 2.48% |
| `hot/steady/baseline/spectralNormInt/500/real_time` | 1,829,146.922 | 1,829,161.736 | +0.00% | 1.042× | 1.047× | 0.17% / 1.34% |
| `hot/steady/baseline/string2float/10000/real_time` | 1,494,116.229 | 1,481,582.668 | -0.84% | 1.729× | 1.700× | 1.23% / 0.13% |
| `hot/steady/baseline/temporaryObjects/1000/real_time` | 76,502.093 | 73,166.362 | -4.36% | 14.398× | 13.251× | 0.17% / 1.86% |
| `hot/steady/baseline/tree/20000/real_time` | 26,092,848.500 | 24,671,279.542 | -5.45% | 4.747× | 4.629× | 6.80% / 0.58% |
| `hot/steady/optimized/arithmetic/1000/real_time` | 786.076 | 787.586 | +0.19% | 2.405× | 2.423× | 1.22% / 0.31% |
| `hot/steady/optimized/arithmetic/4096/real_time` | 3,147.771 | 3,139.937 | -0.25% | 2.391× | 2.342× | 0.35% / 0.40% |
| `hot/steady/optimized/arithmetic/65536/real_time` | 49,995.990 | 49,635.255 | -0.72% | 2.362× | 2.363× | 0.27% / 1.08% |
| `hot/steady/optimized/arrays/1000/real_time` | 2,818.780 | 3,387.097 | +20.16% | 1.270× | 1.524× | 0.91% / 25.15% |
| `hot/steady/optimized/asyncWork/64/real_time` | 39,437.441 | 38,009.173 | -3.62% | 1.771× | 1.718× | 2.77% / 4.84% |
| `hot/steady/optimized/calls/1000/real_time` | 676.064 | 679.316 | +0.48% | 4.413× | 4.440× | 0.21% / 0.01% |
| `hot/steady/optimized/dictionary/500000/real_time` | 32,165,497.722 | 33,955,650.445 | +5.57% | 0.947× | 0.888× | 5.42% / 5.78% |
| `hot/steady/optimized/expLoop/1000000/real_time` | 5,499,469.765 | 5,444,939.549 | -0.99% | 2.258× | 2.242× | 0.23% / 0.93% |
| `hot/steady/optimized/fibonacciLoop/6511134/real_time` | 2,102,044.643 | 2,112,819.392 | +0.51% | 1.005× | 1.002× | 0.63% / 1.38% |
| `hot/steady/optimized/fibonacciRecursive/31/real_time` | 15,743,865.546 | 12,355,754.522 | -21.52% | 3.764× | 2.955× | 24.73% / 0.12% |
| `hot/steady/optimized/fields/1000/real_time` | 25.305 | 25.231 | -0.29% | 0.890× | 0.890× | 0.69% / 1.12% |
| `hot/steady/optimized/float2string/10000/real_time` | 1,417,155.619 | 1,433,575.908 | +1.16% | 1.066× | 1.085× | 2.20% / 1.40% |
| `hot/steady/optimized/host/1000/real_time` | 4,288.697 | 4,287.393 | -0.03% | 4.433× | 4.422× | 0.59% / 0.28% |
| `hot/steady/optimized/interop/1000000/real_time` | 4,245,427.083 | 4,218,746.575 | -0.63% | 4.370× | 4.251× | 0.64% / 0.25% |
| `hot/steady/optimized/mandelbrot/64/real_time` | 385,532.021 | 380,756.812 | -1.24% | 1.039× | 1.039× | 0.94% / 0.46% |
| `hot/steady/optimized/nbodies/33000/real_time` | 2,569,690.315 | 2,556,783.405 | -0.50% | 1.571× | 1.572× | 1.23% / 0.74% |
| `hot/steady/optimized/particles/50000/real_time` | 1,838,252.435 | 1,841,136.247 | +0.16% | 3.268× | 3.291× | 3.69% / 3.07% |
| `hot/steady/optimized/primes/14000/real_time` | 7,075,744.800 | 7,115,615.625 | +0.56% | 1.006× | 1.008× | 0.80% / 0.67% |
| `hot/steady/optimized/queen/8/real_time` | 134,788.666 | 134,037.277 | -0.56% | 2.803× | 2.880× | 2.70% / 0.20% |
| `hot/steady/optimized/sha256/32/real_time` | 282,675.538 | 300,349.177 | +6.25% | 1.743× | 1.865× | 0.08% / 7.75% |
| `hot/steady/optimized/sort/100000/real_time` | 7,386,272.474 | 7,334,105.250 | -0.71% | 1.789× | 1.762× | 1.07% / 0.63% |
| `hot/steady/optimized/spectralNorm/500/real_time` | 1,837,837.013 | 1,853,160.513 | +0.83% | 1.048× | 0.968× | 0.47% / 1.14% |
| `hot/steady/optimized/spectralNormInt/500/real_time` | 1,920,473.310 | 2,446,399.699 | +27.39% | 1.094× | 1.401× | 6.16% / 35.03% |
| `hot/steady/optimized/string2float/10000/real_time` | 1,152,361.421 | 1,121,331.500 | -2.69% | 1.334× | 1.287× | 2.80% / 0.18% |
| `hot/steady/optimized/temporaryObjects/1000/real_time` | 19,396.577 | 19,701.921 | +1.57% | 3.650× | 3.568× | 0.16% / 0.50% |
| `hot/steady/optimized/tree/20000/real_time` | 11,697,940.104 | 12,516,646.739 | +7.00% | 2.128× | 2.349× | 0.03% / 6.15% |
| `interop/aot/runtime/callback` | 30.413 | 28.645 | -5.81% | — | — | 1.88% / 2.13% |
| `interop/aot/runtime/local` | 57.980 | 55.562 | -4.17% | — | — | 0.15% / 0.67% |
| `interop/aot/runtime/object` | 59.145 | 59.436 | +0.49% | — | — | 0.01% / 0.35% |
| `interop/aot/runtime/reentry` | 88.336 | 93.046 | +5.33% | — | — | 0.81% / 0.37% |
| `interop/aot/runtime/scalar` | 23.781 | 22.891 | -3.74% | — | — | 1.99% / 0.66% |
| `interop/aot/saved_callback/boolean` | 1.077 | 1.074 | -0.30% | 1.128× | 1.107× | 0.60% / 0.09% |
| `interop/aot/saved_callback/double` | 0.991 | 1.016 | +2.53% | 0.769× | 1.049× | 0.26% / 2.06% |
| `interop/aot/saved_callback/integer_capture` | 1.089 | 1.088 | -0.10% | 1.126× | 1.118× | 0.61% / 0.20% |
| `interop/aot/saved_callback/integer_max` | 1.101 | 1.077 | -2.22% | 1.142× | 1.115× | 4.13% / 0.32% |
| `interop/aot/saved_callback/integer_negative` | 1.074 | 1.086 | +1.15% | 1.117× | 1.119× | 0.63% / 0.94% |
| `interop/aot/typed/callback` | 5.546 | 5.511 | -0.63% | — | — | 0.45% / 0.10% |
| `interop/aot/typed/local` | 55.194 | 54.103 | -1.98% | — | — | 0.44% / 0.20% |
| `interop/aot/typed/object` | 84.228 | 84.967 | +0.88% | — | — | 1.10% / 0.61% |
| `interop/aot/typed/persistent_callback` | 7.297 | 7.287 | -0.13% | 7.574× | 7.557× | 0.08% / 0.48% |
| `interop/aot/typed/reentry` | 103.336 | 106.397 | +2.96% | — | — | 1.23% / 1.25% |
| `interop/aot/typed/saved_callback` | 1.045 | 1.043 | -0.21% | 1.085× | 1.081× | 1.15% / 0.78% |
| `interop/aot/typed/scalar` | 34.663 | 33.946 | -2.07% | — | — | 1.56% / 0.57% |
| `interop/aot/wrapper/local` | 70.457 | 68.474 | -2.81% | — | — | 0.66% / 0.05% |
| `interop/aot/wrapper/reentry` | 104.472 | 106.283 | +1.73% | — | — | 1.72% / 0.38% |
| `interop/aot/wrapper/scalar` | 36.732 | 35.752 | -2.67% | — | — | 0.11% / 0.27% |
| `interop/async/runtime/completion` | 812.299 | 831.343 | +2.34% | — | — | 0.12% / 2.36% |
| `interop/async/typed/completion` | 838.937 | 813.317 | -3.05% | — | — | 1.62% / 0.14% |
| `interop/cpp/saved_callback/boolean` | 0.955 | 0.970 | +1.53% | 1.000× | 1.000× | 0.32% / 0.47% |
| `interop/cpp/saved_callback/double` | 1.289 | 0.968 | -24.89% | 1.000× | 1.000× | 0.80% / 0.61% |
| `interop/cpp/saved_callback/integer_capture` | 0.967 | 0.973 | +0.64% | 1.000× | 1.000× | 0.11% / 0.64% |
| `interop/cpp/saved_callback/integer_max` | 0.964 | 0.966 | +0.16% | 1.000× | 1.000× | 0.07% / 0.23% |
| `interop/cpp/saved_callback/integer_negative` | 0.961 | 0.971 | +0.96% | 1.000× | 1.000× | 0.26% / 0.65% |
| `interop/cpp/std_function/callback` | 0.963 | 0.964 | +0.09% | 1.000× | 1.000× | 0.27% / 0.21% |
| `interop/forward/reload_optimized/boolean/4096/real_time` | 4.231 | 4.251 | +0.47% | 1.738× | 1.762× | 0.34% / 0.38% |
| `interop/forward/reload_optimized/boolean/64/real_time` | 4.435 | 4.465 | +0.68% | 1.830× | 1.835× | 0.52% / 0.27% |
| `interop/forward/reload_optimized/double/4096/real_time` | 4.495 | 4.510 | +0.34% | 1.266× | 1.250× | 0.04% / 0.25% |
| `interop/forward/reload_optimized/double/64/real_time` | 4.728 | 6.183 | +30.78% | 1.253× | 1.752× | 0.14% / 32.62% |
| `interop/forward/reload_optimized/empty/4096/real_time` | 4.172 | 4.193 | +0.48% | 1.707× | 1.729× | 0.58% / 0.31% |
| `interop/forward/reload_optimized/empty/64/real_time` | 4.359 | 4.371 | +0.27% | 2.734× | 2.722× | 0.19% / 0.04% |
| `interop/forward/reload_optimized/integer/4096/real_time` | 4.243 | 5.623 | +32.51% | 2.007× | 2.694× | 0.02% / 31.78% |
| `interop/forward/reload_optimized/integer/64/real_time` | 4.473 | 4.495 | +0.50% | 3.130× | 3.216× | 0.03% / 0.39% |
| `interop/forward/reload_optimized/string_borrow/4096/real_time` | 5.385 | 5.359 | -0.49% | 5.592× | 5.543× | 0.29% / 0.00% |
| `interop/forward/reload_optimized/string_borrow/64/real_time` | 5.754 | 5.800 | +0.80% | 5.544× | 5.642× | 1.45% / 1.16% |
| `interop/forward/reload_optimized/string_borrow_4096/4096/real_time` | 5.443 | 5.442 | -0.00% | 5.638× | 5.597× | 0.07% / 0.95% |
| `interop/forward/reload_optimized/string_borrow_4096/64/real_time` | 5.825 | 5.829 | +0.07% | 5.649× | 5.657× | 0.92% / 0.69% |
| `interop/forward/reload_optimized/string_borrow_96/4096/real_time` | 5.515 | 5.391 | -2.26% | 5.681× | 5.612× | 3.24% / 1.22% |
| `interop/forward/reload_optimized/string_borrow_96/64/real_time` | 5.808 | 5.763 | -0.77% | 5.642× | 5.610× | 0.79% / 1.10% |
| `interop/jit/runtime/callback` | 27.985 | 28.397 | +1.47% | — | — | 0.10% / 0.56% |
| `interop/jit/runtime/local` | 57.195 | 57.785 | +1.03% | — | — | 0.77% / 0.24% |
| `interop/jit/runtime/object` | 59.402 | 61.880 | +4.17% | — | — | 0.46% / 5.67% |
| `interop/jit/runtime/reentry` | 91.768 | 99.600 | +8.53% | — | — | 0.14% / 2.07% |
| `interop/jit/runtime/scalar` | 28.704 | 23.573 | -17.88% | — | — | 26.17% / 0.58% |
| `interop/jit/typed/callback` | 5.504 | 5.500 | -0.09% | — | — | 0.36% / 0.17% |
| `interop/jit/typed/local` | 55.982 | 56.489 | +0.91% | — | — | 0.30% / 3.28% |
| `interop/jit/typed/object` | 84.316 | 84.734 | +0.50% | — | — | 0.35% / 0.65% |
| `interop/jit/typed/reentry` | 104.287 | 107.189 | +2.78% | — | — | 1.30% / 1.10% |
| `interop/jit/typed/scalar` | 35.334 | 34.686 | -1.83% | — | — | 0.32% / 0.16% |
| `interop/reload_optimized/saved_callback/boolean` | 1.079 | 1.095 | +1.48% | 1.129× | 1.129× | 0.88% / 2.54% |
| `interop/reload_optimized/saved_callback/double` | 0.968 | 1.000 | +3.36% | 0.751× | 1.034× | 0.14% / 0.31% |
| `interop/reload_optimized/saved_callback/integer_capture` | 1.086 | 1.107 | +1.97% | 1.123× | 1.138× | 0.30% / 0.17% |
| `interop/reload_optimized/saved_callback/integer_max` | 1.058 | 1.063 | +0.45% | 1.097× | 1.100× | 0.59% / 0.90% |
| `interop/reload_optimized/saved_callback/integer_negative` | 1.045 | 1.059 | +1.39% | 1.087× | 1.091× | 0.53% / 0.21% |
| `interop/reload_optimized/typed/persistent_callback` | 7.328 | 7.335 | +0.09% | 7.607× | 7.607× | 0.09% / 0.25% |
| `interop/reload_optimized/typed/saved_callback` | 1.049 | 1.060 | +1.00% | 1.089× | 1.099× | 0.60% / 0.12% |
| `interop/type/bound` | 27.385 | 34.651 | +26.53% | — | — | 1.89% / 2.23% |
| `interop/type/cold` | 5,168.799 | 5,032.186 | -2.64% | — | — | 1.29% / 0.40% |
| `interop/values/runtime/array_reference` | 7.204 | 7.174 | -0.41% | — | — | 0.56% / 0.37% |
| `interop/values/runtime/array_scalar` | 3.691 | 3.721 | +0.81% | — | — | 0.35% / 0.13% |
| `interop/values/runtime/persistent` | 18.292 | 18.079 | -1.16% | — | — | 1.23% / 1.03% |
| `interop/values/runtime/string_borrow` | 3.582 | 3.635 | +1.47% | — | — | 0.45% / 1.66% |
| `interop/values/runtime/string_copy` | 34.239 | 34.936 | +2.03% | — | — | 0.97% / 0.09% |
| `interop/values/typed/array_reference` | 7.421 | 7.496 | +1.00% | — | — | 0.09% / 2.00% |
| `interop/values/typed/array_scalar` | 8.342 | 8.355 | +0.16% | — | — | 0.55% / 0.15% |
| `interop/values/typed/persistent` | 18.312 | 18.095 | -1.19% | — | — | 0.03% / 0.18% |
| `interop/values/typed/string_borrow` | 3.554 | 4.549 | +27.99% | — | — | 1.08% / 29.96% |
| `interop/values/typed/string_copy` | 34.554 | 34.877 | +0.93% | — | — | 0.36% / 0.05% |
| `native/aot/boolean/4096/real_time` | 1.131 | 1.133 | +0.17% | 0.464× | 0.469× | 0.26% / 1.01% |
| `native/aot/boolean/64/real_time` | 1.375 | 1.373 | -0.11% | 0.567× | 0.564× | 0.34% / 0.04% |
| `native/aot/double/4096/real_time` | 0.966 | 1.000 | +3.47% | 0.272× | 0.277× | 0.39% / 3.72% |
| `native/aot/double/64/real_time` | 1.251 | 1.228 | -1.84% | 0.332× | 0.348× | 0.52% / 0.75% |
| `native/aot/empty/4096/real_time` | 1.142 | 1.180 | +3.34% | 0.467× | 0.487× | 0.77% / 6.42% |
| `native/aot/empty/64/real_time` | 1.381 | 1.369 | -0.87% | 0.866× | 0.852× | 0.23% / 2.45% |
| `native/aot/integer/4096/real_time` | 1.189 | 1.120 | -5.83% | 0.562× | 0.537× | 0.46% / 0.10% |
| `native/aot/integer/64/real_time` | 1.369 | 1.365 | -0.28% | 0.958× | 0.977× | 0.12% / 0.29% |
| `native/aot/string_borrow/4096/real_time` | 1.150 | 1.131 | -1.65% | 1.194× | 1.170× | 2.04% / 0.11% |
| `native/aot/string_borrow/64/real_time` | 1.581 | 1.374 | -13.12% | 1.523× | 1.336× | 8.42% / 1.28% |
| `native/aot/string_borrow_4096/4096/real_time` | 1.130 | 1.125 | -0.49% | 1.171× | 1.156× | 0.08% / 0.20% |
| `native/aot/string_borrow_4096/64/real_time` | 1.383 | 1.372 | -0.85% | 1.341× | 1.331× | 0.02% / 0.01% |
| `native/aot/string_borrow_96/4096/real_time` | 1.127 | 1.144 | +1.46% | 1.161× | 1.191× | 0.38% / 1.35% |
| `native/aot/string_borrow_96/64/real_time` | 1.384 | 1.364 | -1.47% | 1.344× | 1.327× | 0.57% / 0.22% |
| `native/aot_control/boolean/4096/real_time` | 0.603 | 0.485 | -19.54% | 0.247× | 0.201× | 26.66% / 0.19% |
| `native/aot_control/boolean/64/real_time` | 0.186 | 0.232 | +24.67% | 0.077× | 0.095× | 0.30% / 0.20% |
| `native/aot_control/double/4096/real_time` | 0.955 | 0.974 | +2.00% | 0.269× | 0.270× | 0.23% / 4.02% |
| `native/aot_control/double/64/real_time` | 0.235 | 0.234 | -0.29% | 0.062× | 0.066× | 0.24% / 0.52% |
| `native/aot_control/empty/4096/real_time` | 0.600 | 0.542 | -9.62% | 0.245× | 0.223× | 26.12% / 0.75% |
| `native/aot_control/empty/64/real_time` | 0.191 | 0.229 | +19.64% | 0.120× | 0.143× | 2.17% / 1.21% |
| `native/aot_control/integer/4096/real_time` | 0.489 | 0.543 | +11.04% | 0.231× | 0.260× | 0.42% / 0.79% |
| `native/aot_control/integer/64/real_time` | 0.188 | 0.231 | +23.18% | 0.131× | 0.165× | 0.00% / 0.52% |
| `native/aot_control/string_borrow/4096/real_time` | 0.333 | 0.332 | -0.22% | 0.346× | 0.344× | 0.93% / 0.55% |
| `native/aot_control/string_borrow/64/real_time` | 0.234 | 0.233 | -0.42% | 0.225× | 0.226× | 0.82% / 0.26% |
| `native/aot_control/string_borrow_4096/4096/real_time` | 0.332 | 0.341 | +2.99% | 0.343× | 0.351× | 0.28% / 2.18% |
| `native/aot_control/string_borrow_4096/64/real_time` | 0.233 | 0.234 | +0.42% | 0.226× | 0.227× | 0.39% / 2.06% |
| `native/aot_control/string_borrow_96/4096/real_time` | 0.341 | 0.330 | -3.21% | 0.352× | 0.344× | 3.97% / 0.00% |
| `native/aot_control/string_borrow_96/64/real_time` | 0.235 | 0.232 | -0.95% | 0.228× | 0.226× | 1.97% / 0.87% |
| `native/baseline/boolean/4096/real_time` | 1.954 | 1.926 | -1.41% | 0.803× | 0.798× | 0.38% / 0.22% |
| `native/baseline/boolean/64/real_time` | 2.200 | 2.195 | -0.24% | 0.907× | 0.902× | 0.73% / 1.13% |
| `native/baseline/double/4096/real_time` | 4.543 | 4.520 | -0.49% | 1.279× | 1.253× | 0.71% / 0.35% |
| `native/baseline/double/64/real_time` | 4.632 | 4.627 | -0.11% | 1.228× | 1.311× | 0.57% / 0.45% |
| `native/baseline/empty/4096/real_time` | 1.938 | 1.934 | -0.20% | 0.793× | 0.797× | 0.11% / 0.19% |
| `native/baseline/empty/64/real_time` | 2.171 | 2.188 | +0.75% | 1.362× | 1.362× | 0.06% / 1.72% |
| `native/baseline/integer/4096/real_time` | 1.933 | 1.932 | -0.05% | 0.914× | 0.926× | 0.42% / 0.60% |
| `native/baseline/integer/64/real_time` | 2.179 | 2.156 | -1.05% | 1.525× | 1.543× | 0.30% / 0.15% |
| `native/baseline/string_borrow/4096/real_time` | 4.687 | 4.635 | -1.09% | 4.866× | 4.795× | 0.57% / 0.08% |
| `native/baseline/string_borrow/64/real_time` | 4.840 | 4.862 | +0.47% | 4.663× | 4.730× | 0.10% / 0.62% |
| `native/baseline/string_borrow_4096/4096/real_time` | 4.820 | 4.607 | -4.43% | 4.994× | 4.738× | 5.25% / 0.28% |
| `native/baseline/string_borrow_4096/64/real_time` | 4.813 | 4.819 | +0.11% | 4.668× | 4.676× | 0.17% / 0.46% |
| `native/baseline/string_borrow_96/4096/real_time` | 4.647 | 4.664 | +0.35% | 4.787× | 4.856× | 0.54% / 0.25% |
| `native/baseline/string_borrow_96/64/real_time` | 4.923 | 4.830 | -1.87% | 4.782× | 4.702× | 1.01% / 0.57% |
| `native/baseline_control/boolean/4096/real_time` | 0.507 | 0.495 | -2.27% | 0.208× | 0.205× | 4.31% / 1.91% |
| `native/baseline_control/boolean/64/real_time` | 0.186 | 0.186 | -0.20% | 0.077× | 0.076× | 0.78% / 0.75% |
| `native/baseline_control/double/4096/real_time` | 0.962 | 1.002 | +4.14% | 0.271× | 0.278× | 0.82% / 3.19% |
| `native/baseline_control/double/64/real_time` | 0.277 | 0.178 | -35.98% | 0.074× | 0.050× | 50.59% / 1.10% |
| `native/baseline_control/empty/4096/real_time` | 0.542 | 0.544 | +0.40% | 0.222× | 0.225× | 0.65% / 0.38% |
| `native/baseline_control/empty/64/real_time` | 0.197 | 0.253 | +27.97% | 0.124× | 0.157× | 0.56% / 34.06% |
| `native/baseline_control/integer/4096/real_time` | 0.559 | 0.558 | -0.18% | 0.264× | 0.267× | 4.02% / 3.36% |
| `native/baseline_control/integer/64/real_time` | 0.186 | 0.194 | +4.43% | 0.130× | 0.139× | 0.23% / 6.03% |
| `native/baseline_control/string_borrow/4096/real_time` | 0.334 | 0.332 | -0.55% | 0.347× | 0.343× | 0.32% / 0.24% |
| `native/baseline_control/string_borrow/64/real_time` | 0.179 | 0.185 | +3.25% | 0.173× | 0.180× | 1.92% / 5.96% |
| `native/baseline_control/string_borrow_4096/4096/real_time` | 0.333 | 0.337 | +1.16% | 0.345× | 0.346× | 0.15% / 2.74% |
| `native/baseline_control/string_borrow_4096/64/real_time` | 0.179 | 0.178 | -0.18% | 0.173× | 0.173× | 0.20% / 1.21% |
| `native/baseline_control/string_borrow_96/4096/real_time` | 0.334 | 0.395 | +18.28% | 0.344× | 0.412× | 1.04% / 23.07% |
| `native/baseline_control/string_borrow_96/64/real_time` | 0.179 | 0.178 | -0.56% | 0.174× | 0.173× | 0.21% / 1.34% |
| `native/cpp/boolean/4096/real_time` | 2.435 | 2.413 | -0.91% | 1.000× | 1.000× | 1.19% / 1.04% |
| `native/cpp/boolean/64/real_time` | 2.424 | 2.433 | +0.38% | 1.000× | 1.000× | 0.27% / 1.85% |
| `native/cpp/double/4096/real_time` | 3.552 | 3.609 | +1.62% | 1.000× | 1.000× | 0.83% / 2.33% |
| `native/cpp/double/64/real_time` | 3.772 | 3.528 | -6.47% | 1.000× | 1.000× | 3.02% / 0.39% |
| `native/cpp/empty/4096/real_time` | 2.444 | 2.425 | -0.80% | 1.000× | 1.000× | 1.26% / 0.01% |
| `native/cpp/empty/64/real_time` | 1.595 | 1.606 | +0.71% | 1.000× | 1.000× | 0.54% / 0.21% |
| `native/cpp/integer/4096/real_time` | 2.114 | 2.087 | -1.30% | 1.000× | 1.000× | 0.24% / 0.68% |
| `native/cpp/integer/64/real_time` | 1.429 | 1.398 | -2.20% | 1.000× | 1.000× | 2.68% / 0.28% |
| `native/cpp/string_borrow/4096/real_time` | 0.963 | 0.967 | +0.39% | 1.000× | 1.000× | 0.14% / 1.15% |
| `native/cpp/string_borrow/64/real_time` | 1.038 | 1.028 | -0.95% | 1.000× | 1.000× | 0.80% / 0.70% |
| `native/cpp/string_borrow_4096/4096/real_time` | 0.965 | 0.972 | +0.74% | 1.000× | 1.000× | 0.30% / 1.00% |
| `native/cpp/string_borrow_4096/64/real_time` | 1.031 | 1.030 | -0.08% | 1.000× | 1.000× | 0.20% / 0.90% |
| `native/cpp/string_borrow_96/4096/real_time` | 0.971 | 0.960 | -1.06% | 1.000× | 1.000× | 0.73% / 0.00% |
| `native/cpp/string_borrow_96/64/real_time` | 1.029 | 1.027 | -0.20% | 1.000× | 1.000× | 0.02% / 0.01% |
| `native/cpp_indirect/boolean/4096/real_time` | 2.476 | 2.478 | +0.04% | 1.017× | 1.027× | 0.10% / 1.21% |
| `native/cpp_indirect/boolean/64/real_time` | 2.419 | 2.414 | -0.22% | 0.998× | 0.992× | 1.04% / 0.66% |
| `native/cpp_indirect/double/4096/real_time` | 3.542 | 4.187 | +18.22% | 0.997× | 1.160× | 0.59% / 21.93% |
| `native/cpp_indirect/double/64/real_time` | 3.534 | 3.558 | +0.68% | 0.937× | 1.008× | 0.07% / 1.61% |
| `native/cpp_indirect/empty/4096/real_time` | 2.452 | 2.453 | +0.04% | 1.003× | 1.012× | 0.33% / 0.06% |
| `native/cpp_indirect/empty/64/real_time` | 1.622 | 2.036 | +25.51% | 1.017× | 1.268× | 0.69% / 31.30% |
| `native/cpp_indirect/integer/4096/real_time` | 2.132 | 2.281 | +7.03% | 1.008× | 1.093× | 0.51% / 8.34% |
| `native/cpp_indirect/integer/64/real_time` | 1.407 | 1.409 | +0.11% | 0.985× | 1.008× | 0.35% / 0.57% |
| `native/cpp_indirect/string_borrow/4096/real_time` | 0.977 | 0.958 | -1.96% | 1.015× | 0.991× | 2.06% / 0.29% |
| `native/cpp_indirect/string_borrow/64/real_time` | 1.029 | 1.035 | +0.62% | 0.991× | 1.007× | 0.04% / 0.08% |
| `native/cpp_indirect/string_borrow_4096/4096/real_time` | 1.006 | 0.969 | -3.68% | 1.042× | 0.997× | 5.48% / 0.36% |
| `native/cpp_indirect/string_borrow_4096/64/real_time` | 1.041 | 1.045 | +0.39% | 1.010× | 1.015× | 0.90% / 1.68% |
| `native/cpp_indirect/string_borrow_96/4096/real_time` | 0.971 | 0.970 | -0.09% | 1.000× | 1.010× | 1.07% / 1.37% |
| `native/cpp_indirect/string_borrow_96/64/real_time` | 1.031 | 1.021 | -0.93% | 1.002× | 0.994× | 0.45% / 0.09% |
| `native/optimized/boolean/4096/real_time` | 1.951 | 1.936 | -0.79% | 0.801× | 0.802× | 1.60% / 0.19% |
| `native/optimized/boolean/64/real_time` | 2.207 | 2.190 | -0.79% | 0.911× | 0.900× | 0.37% / 0.38% |
| `native/optimized/double/4096/real_time` | 4.526 | 4.523 | -0.06% | 1.274× | 1.253× | 0.85% / 0.09% |
| `native/optimized/double/64/real_time` | 4.659 | 4.628 | -0.68% | 1.235× | 1.312× | 1.54% / 0.24% |
| `native/optimized/empty/4096/real_time` | 1.928 | 1.935 | +0.36% | 0.789× | 0.798× | 0.19% / 0.66% |
| `native/optimized/empty/64/real_time` | 2.228 | 2.169 | -2.66% | 1.397× | 1.350× | 3.26% / 0.33% |
| `native/optimized/integer/4096/real_time` | 1.952 | 1.951 | -0.06% | 0.923× | 0.935× | 1.28% / 0.27% |
| `native/optimized/integer/64/real_time` | 2.453 | 2.165 | -11.73% | 1.716× | 1.549× | 15.48% / 0.80% |
| `native/optimized/string_borrow/4096/real_time` | 4.680 | 4.628 | -1.13% | 4.860× | 4.787× | 0.71% / 0.19% |
| `native/optimized/string_borrow/64/real_time` | 4.846 | 4.811 | -0.72% | 4.669× | 4.680× | 0.00% / 0.03% |
| `native/optimized/string_borrow_4096/4096/real_time` | 4.650 | 4.933 | +6.07% | 4.818× | 5.073× | 0.40% / 6.32% |
| `native/optimized/string_borrow_4096/64/real_time` | 4.895 | 4.811 | -1.72% | 4.747× | 4.669× | 2.20% / 0.45% |
| `native/optimized/string_borrow_96/4096/real_time` | 6.203 | 4.626 | -25.43% | 6.389× | 4.816× | 35.12% / 0.01% |
| `native/optimized/string_borrow_96/64/real_time` | 4.868 | 4.847 | -0.43% | 4.729× | 4.718× | 0.33% / 0.16% |
| `native/optimized_control/boolean/4096/real_time` | 0.490 | 0.487 | -0.64% | 0.201× | 0.202× | 0.20% / 0.27% |
| `native/optimized_control/boolean/64/real_time` | 0.187 | 0.186 | -0.22% | 0.077× | 0.077× | 0.31% / 0.40% |
| `native/optimized_control/double/4096/real_time` | 0.956 | 0.959 | +0.35% | 0.269× | 0.266× | 0.34% / 1.25% |
| `native/optimized_control/double/64/real_time` | 0.179 | 0.179 | +0.25% | 0.047× | 0.051× | 0.60% / 0.49% |
| `native/optimized_control/empty/4096/real_time` | 0.545 | 0.537 | -1.41% | 0.223× | 0.222× | 1.42% / 0.04% |
| `native/optimized_control/empty/64/real_time` | 0.187 | 0.187 | +0.01% | 0.117× | 0.116× | 0.44% / 0.70% |
| `native/optimized_control/integer/4096/real_time` | 0.543 | 0.550 | +1.21% | 0.257× | 0.263× | 0.32% / 0.80% |
| `native/optimized_control/integer/64/real_time` | 0.188 | 0.186 | -1.07% | 0.132× | 0.133× | 0.87% / 0.68% |
| `native/optimized_control/string_borrow/4096/real_time` | 0.335 | 0.333 | -0.55% | 0.347× | 0.344× | 0.98% / 1.00% |
| `native/optimized_control/string_borrow/64/real_time` | 0.180 | 0.178 | -0.76% | 0.173× | 0.173× | 0.26% / 0.02% |
| `native/optimized_control/string_borrow_4096/4096/real_time` | 0.332 | 0.332 | -0.15% | 0.344× | 0.341× | 0.40% / 0.53% |
| `native/optimized_control/string_borrow_4096/64/real_time` | 0.182 | 0.177 | -2.44% | 0.176× | 0.172× | 2.84% / 0.19% |
| `native/optimized_control/string_borrow_96/4096/real_time` | 0.335 | 0.331 | -0.95% | 0.345× | 0.345× | 0.35% / 0.05% |
| `native/optimized_control/string_borrow_96/64/real_time` | 0.178 | 0.176 | -1.04% | 0.173× | 0.171× | 0.61% / 0.11% |
| `native_class/aot/class/alternating/real_time` | 3.177 | 1.668 | -47.48% | 1.452× | 0.790× | 65.44% / 0.60% |
| `native_class/aot/class/boolean/real_time` | 1.151 | 1.105 | -4.05% | 0.553× | 0.548× | 2.07% / 0.93% |
| `native_class/aot/class/bound/real_time` | 1.959 | 1.930 | -1.45% | — | — | 0.14% / 0.97% |
| `native_class/aot/class/construct/real_time` | 117.583 | 107.386 | -8.67% | — | — | 0.22% / 1.11% |
| `native_class/aot/class/dynamic/real_time` | 278.427 | 138.332 | -50.32% | — | — | 71.25% / 0.46% |
| `native_class/aot/class/empty/real_time` | 1.061 | 1.076 | +1.42% | 0.509× | 0.527× | 1.38% / 5.25% |
| `native_class/aot/class/failure/real_time` | 332.326 | 326.874 | -1.64% | — | — | 1.29% / 0.56% |
| `native_class/aot/class/floating/real_time` | 1.368 | 1.337 | -2.27% | 0.388× | 0.391× | 0.00% / 1.10% |
| `native_class/aot/class/gc/real_time` | 1,337.337 | 1,243.160 | -7.04% | — | — | 7.01% / 1.16% |
| `native_class/aot/class/getter/real_time` | 1.059 | 1.062 | +0.26% | 0.823× | 0.846× | 0.81% / 3.29% |
| `native_class/aot/class/integer/real_time` | 1.055 | 1.036 | -1.81% | 0.811× | 0.833× | 0.14% / 0.12% |
| `native_class/aot/class/nullable/real_time` | 1.959 | 1.793 | -8.51% | 0.932× | 0.887× | 1.60% / 0.02% |
| `native_class/aot/class/reentry/real_time` | 13.704 | 13.800 | +0.70% | — | — | 0.11% / 0.26% |
| `native_class/aot/class/repeated/real_time` | 41.361 | 40.786 | -1.39% | — | — | 0.56% / 0.95% |
| `native_class/aot/class/setter/real_time` | 1.054 | 1.020 | -3.30% | 0.503× | 0.490× | 0.57% / 0.03% |
| `native_class/aot/class/shared/real_time` | 139.316 | 136.070 | -2.33% | — | — | 1.37% / 0.96% |
| `native_class/aot/class/static/real_time` | 0.818 | 0.792 | -3.19% | 0.527× | 0.631× | 0.33% / 0.39% |
| `native_class/aot/class/unique/real_time` | 138.663 | 134.168 | -3.24% | — | — | 0.17% / 0.77% |
| `native_class/aot/class/void/real_time` | 1.052 | 1.044 | -0.82% | 0.503× | 0.514× | 0.41% / 2.86% |
| `native_class/aot/free/alternating/real_time` | 1.696 | 1.679 | -1.01% | 0.775× | 0.795× | 0.15% / 0.77% |
| `native_class/aot/free/boolean/real_time` | 1.170 | 1.111 | -5.00% | 0.562× | 0.552× | 4.05% / 0.83% |
| `native_class/aot/free/bound/real_time` | 5.352 | 5.366 | +0.27% | — | — | 0.10% / 0.75% |
| `native_class/aot/free/construct/real_time` | 119.844 | 106.808 | -10.88% | — | — | 1.08% / 1.12% |
| `native_class/aot/free/dynamic/real_time` | 117.823 | 115.199 | -2.23% | — | — | 0.24% / 1.05% |
| `native_class/aot/free/empty/real_time` | 1.377 | 1.030 | -25.17% | 0.661× | 0.505× | 32.59% / 0.48% |
| `native_class/aot/free/failure/real_time` | 328.731 | 325.003 | -1.13% | — | — | 0.38% / 0.59% |
| `native_class/aot/free/floating/real_time` | 1.367 | 1.351 | -1.17% | 0.388× | 0.395× | 0.45% / 0.16% |
| `native_class/aot/free/gc/real_time` | 1,259.056 | 1,250.127 | -0.71% | — | — | 2.32% / 1.94% |
| `native_class/aot/free/getter/real_time` | 1.071 | 1.040 | -2.93% | 0.832× | 0.828× | 0.01% / 0.83% |
| `native_class/aot/free/integer/real_time` | 1.057 | 1.027 | -2.80% | 0.812× | 0.826× | 0.65% / 1.33% |
| `native_class/aot/free/nullable/real_time` | 1.953 | 1.811 | -7.27% | 0.929× | 0.897× | 2.13% / 1.12% |
| `native_class/aot/free/reentry/real_time` | 13.714 | 13.877 | +1.19% | — | — | 0.27% / 0.33% |
| `native_class/aot/free/repeated/real_time` | 41.346 | 40.610 | -1.78% | — | — | 0.04% / 0.32% |
| `native_class/aot/free/setter/real_time` | 1.053 | 1.021 | -3.11% | 0.502× | 0.490× | 0.00% / 0.03% |
| `native_class/aot/free/shared/real_time` | 139.230 | 136.702 | -1.82% | — | — | 0.17% / 0.08% |
| `native_class/aot/free/static/real_time` | 0.810 | 0.794 | -2.03% | 0.522× | 0.633× | 0.15% / 1.11% |
| `native_class/aot/free/unique/real_time` | 136.657 | 131.004 | -4.14% | — | — | 1.01% / 0.90% |
| `native_class/aot/free/void/real_time` | 1.058 | 1.020 | -3.62% | 0.506× | 0.503× | 0.20% / 0.17% |
| `native_class/baseline/class/alternating/real_time` | 2.265 | 2.227 | -1.70% | 1.036× | 1.054× | 0.57% / 0.12% |
| `native_class/baseline/class/boolean/real_time` | 1.481 | 1.449 | -2.14% | 0.712× | 0.719× | 0.33% / 0.21% |
| `native_class/baseline/class/bound/real_time` | 17.397 | 13.576 | -21.96% | — | — | 31.33% / 0.20% |
| `native_class/baseline/class/construct/real_time` | 117.471 | 107.162 | -8.78% | — | — | 1.81% / 1.11% |
| `native_class/baseline/class/dynamic/real_time` | 138.224 | 140.526 | +1.67% | — | — | 0.51% / 0.85% |
| `native_class/baseline/class/empty/real_time` | 1.615 | 1.654 | +2.42% | 0.775× | 0.811× | 0.05% / 1.66% |
| `native_class/baseline/class/failure/real_time` | 329.341 | 326.555 | -0.85% | — | — | 0.64% / 0.23% |
| `native_class/baseline/class/floating/real_time` | 1.492 | 1.428 | -4.33% | 0.423× | 0.417× | 3.77% / 0.11% |
| `native_class/baseline/class/gc/real_time` | 1,257.135 | 1,219.010 | -3.03% | — | — | 0.46% / 1.03% |
| `native_class/baseline/class/getter/real_time` | 1.473 | 1.437 | -2.43% | 1.145× | 1.145× | 0.34% / 0.70% |
| `native_class/baseline/class/integer/real_time` | 1.472 | 1.447 | -1.75% | 1.132× | 1.163× | 0.37% / 0.42% |
| `native_class/baseline/class/nullable/real_time` | 2.436 | 2.395 | -1.68% | 1.159× | 1.185× | 0.68% / 0.70% |
| `native_class/baseline/class/reentry/real_time` | 15.589 | 15.439 | -0.97% | — | — | 0.33% / 0.47% |
| `native_class/baseline/class/repeated/real_time` | 41.923 | 42.053 | +0.31% | — | — | 0.12% / 1.88% |
| `native_class/baseline/class/setter/real_time` | 1.627 | 1.606 | -1.30% | 0.775× | 0.771× | 0.06% / 0.89% |
| `native_class/baseline/class/shared/real_time` | 139.569 | 133.392 | -4.43% | — | — | 2.25% / 1.97% |
| `native_class/baseline/class/static/real_time` | 1.315 | 1.276 | -2.90% | 0.847× | 1.017× | 1.53% / 0.11% |
| `native_class/baseline/class/unique/real_time` | 142.133 | 131.229 | -7.67% | — | — | 4.32% / 0.35% |
| `native_class/baseline/class/void/real_time` | 2.026 | 1.606 | -20.73% | 0.968× | 0.792× | 28.99% / 0.87% |
| `native_class/baseline/free/alternating/real_time` | 2.279 | 2.207 | -3.18% | 1.042× | 1.045× | 0.95% / 1.44% |
| `native_class/baseline/free/boolean/real_time` | 1.646 | 1.602 | -2.64% | 0.791× | 0.795× | 1.56% / 0.87% |
| `native_class/baseline/free/bound/real_time` | 18.864 | 14.195 | -24.75% | — | — | 31.48% / 0.28% |
| `native_class/baseline/free/construct/real_time` | 118.929 | 107.863 | -9.30% | — | — | 0.94% / 1.13% |
| `native_class/baseline/free/dynamic/real_time` | 117.550 | 117.324 | -0.19% | — | — | 0.04% / 0.57% |
| `native_class/baseline/free/empty/real_time` | 1.636 | 1.597 | -2.41% | 0.785× | 0.783× | 0.75% / 0.12% |
| `native_class/baseline/free/failure/real_time` | 326.814 | 323.365 | -1.06% | — | — | 0.23% / 2.13% |
| `native_class/baseline/free/floating/real_time` | 1.467 | 1.428 | -2.67% | 0.416× | 0.417× | 1.34% / 0.14% |
| `native_class/baseline/free/gc/real_time` | 1,244.988 | 1,224.098 | -1.68% | — | — | 0.18% / 0.03% |
| `native_class/baseline/free/getter/real_time` | 1.477 | 1.450 | -1.85% | 1.148× | 1.155× | 0.36% / 0.29% |
| `native_class/baseline/free/integer/real_time` | 1.488 | 1.451 | -2.45% | 1.144× | 1.166× | 1.90% / 0.52% |
| `native_class/baseline/free/nullable/real_time` | 2.428 | 2.396 | -1.29% | 1.155× | 1.186× | 0.12% / 0.84% |
| `native_class/baseline/free/reentry/real_time` | 15.632 | 15.468 | -1.05% | — | — | 0.25% / 0.10% |
| `native_class/baseline/free/repeated/real_time` | 46.689 | 57.536 | +23.23% | — | — | 12.48% / 39.87% |
| `native_class/baseline/free/setter/real_time` | 1.623 | 1.586 | -2.27% | 0.773× | 0.762× | 0.19% / 1.16% |
| `native_class/baseline/free/shared/real_time` | 146.862 | 135.520 | -7.72% | — | — | 1.54% / 0.34% |
| `native_class/baseline/free/static/real_time` | 1.300 | 1.283 | -1.34% | 0.838× | 1.023× | 0.19% / 1.20% |
| `native_class/baseline/free/unique/real_time` | 136.551 | 132.836 | -2.72% | — | — | 0.72% / 4.57% |
| `native_class/baseline/free/void/real_time` | 3.337 | 1.597 | -52.14% | 1.595× | 0.787× | 72.71% / 0.17% |
| `native_class/cpp_adapter/alternating/real_time` | 2.234 | 2.110 | -5.54% | 1.021× | 0.999× | 0.07% / 0.06% |
| `native_class/cpp_adapter/boolean/real_time` | 2.071 | 2.012 | -2.82% | 0.995× | 0.999× | 0.11% / 0.03% |
| `native_class/cpp_adapter/empty/real_time` | 2.067 | 2.013 | -2.60% | 0.992× | 0.987× | 0.10% / 0.48% |
| `native_class/cpp_adapter/floating/real_time` | 6.847 | 3.416 | -50.11% | 1.941× | 0.999× | 68.45% / 0.00% |
| `native_class/cpp_adapter/getter/real_time` | 1.285 | 1.268 | -1.36% | 0.999× | 1.010× | 0.35% / 1.40% |
| `native_class/cpp_adapter/integer/real_time` | 1.288 | 1.258 | -2.33% | 0.990× | 1.011× | 0.04% / 1.60% |
| `native_class/cpp_adapter/nullable/real_time` | 2.084 | 2.045 | -1.88% | 0.991× | 1.012× | 0.42% / 1.31% |
| `native_class/cpp_adapter/setter/real_time` | 2.093 | 2.054 | -1.85% | 0.997× | 0.987× | 0.66% / 1.52% |
| `native_class/cpp_adapter/static/real_time` | 1.285 | 1.258 | -2.13% | 0.828× | 1.002× | 0.42% / 1.52% |
| `native_class/cpp_adapter/void/real_time` | 2.123 | 2.026 | -4.57% | 1.015× | 0.998× | 2.50% / 0.07% |
| `native_class/cpp_pointer/alternating/real_time` | 2.187 | 2.113 | -3.42% | 1.000× | 1.000× | 0.30% / 0.24% |
| `native_class/cpp_pointer/boolean/real_time` | 2.081 | 2.015 | -3.18% | 1.000× | 1.000× | 0.17% / 0.23% |
| `native_class/cpp_pointer/empty/real_time` | 2.083 | 2.040 | -2.07% | 1.000× | 1.000× | 0.81% / 0.18% |
| `native_class/cpp_pointer/floating/real_time` | 3.527 | 3.421 | -3.01% | 1.000× | 1.000× | 0.37% / 0.11% |
| `native_class/cpp_pointer/getter/real_time` | 1.287 | 1.255 | -2.47% | 1.000× | 1.000× | 0.78% / 1.27% |
| `native_class/cpp_pointer/integer/real_time` | 1.301 | 1.244 | -4.35% | 1.000× | 1.000× | 2.00% / 0.03% |
| `native_class/cpp_pointer/nullable/real_time` | 2.102 | 2.021 | -3.86% | 1.000× | 1.000× | 0.87% / 0.08% |
| `native_class/cpp_pointer/setter/real_time` | 2.098 | 2.081 | -0.81% | 1.000× | 1.000× | 0.09% / 2.26% |
| `native_class/cpp_pointer/static/real_time` | 1.553 | 1.255 | -19.18% | 1.000× | 1.000× | 24.17% / 1.29% |
| `native_class/cpp_pointer/void/real_time` | 2.093 | 2.030 | -3.02% | 1.000× | 1.000× | 0.72% / 0.17% |
| `native_class/optimized/class/alternating/real_time` | 2.259 | 2.213 | -2.00% | 1.033× | 1.048× | 0.32% / 0.03% |
| `native_class/optimized/class/boolean/real_time` | 1.619 | 1.596 | -1.37% | 0.778× | 0.792× | 0.54% / 0.12% |
| `native_class/optimized/class/bound/real_time` | 1.976 | 1.361 | -31.12% | — | — | 42.77% / 0.12% |
| `native_class/optimized/class/construct/real_time` | 116.642 | 106.598 | -8.61% | — | — | 0.51% / 2.13% |
| `native_class/optimized/class/dynamic/real_time` | 139.014 | 139.992 | +0.70% | — | — | 0.32% / 0.05% |
| `native_class/optimized/class/empty/real_time` | 1.617 | 1.594 | -1.41% | 0.776× | 0.781× | 0.53% / 0.31% |
| `native_class/optimized/class/failure/real_time` | 329.488 | 326.502 | -0.91% | — | — | 0.57% / 0.17% |
| `native_class/optimized/class/floating/real_time` | 1.452 | 1.444 | -0.56% | 0.412× | 0.422× | 0.54% / 1.40% |
| `native_class/optimized/class/gc/real_time` | 1,227.990 | 1,224.902 | -0.25% | — | — | 0.01% / 0.53% |
| `native_class/optimized/class/getter/real_time` | 1.477 | 1.446 | -2.07% | 1.147× | 1.152× | 0.02% / 0.02% |
| `native_class/optimized/class/integer/real_time` | 1.620 | 1.594 | -1.60% | 1.246× | 1.281× | 0.27% / 0.73% |
| `native_class/optimized/class/nullable/real_time` | 2.425 | 2.384 | -1.69% | 1.154× | 1.180× | 0.13% / 0.31% |
| `native_class/optimized/class/reentry/real_time` | 15.681 | 15.461 | -1.40% | — | — | 0.30% / 0.22% |
| `native_class/optimized/class/repeated/real_time` | 41.595 | 41.044 | -1.32% | — | — | 0.14% / 0.03% |
| `native_class/optimized/class/setter/real_time` | 1.624 | 1.598 | -1.62% | 0.774× | 0.768× | 0.37% / 0.30% |
| `native_class/optimized/class/shared/real_time` | 141.500 | 136.189 | -3.75% | — | — | 0.54% / 3.12% |
| `native_class/optimized/class/static/real_time` | 1.301 | 1.277 | -1.86% | 0.838× | 1.017× | 0.46% / 0.22% |
| `native_class/optimized/class/unique/real_time` | 137.873 | 130.368 | -5.44% | — | — | 2.10% / 1.84% |
| `native_class/optimized/class/void/real_time` | 1.659 | 1.596 | -3.76% | 0.793× | 0.787× | 0.48% / 0.37% |
| `native_class/optimized/free/alternating/real_time` | 2.256 | 2.232 | -1.07% | 1.031× | 1.056× | 0.29% / 0.58% |
| `native_class/optimized/free/boolean/real_time` | 1.474 | 1.458 | -1.06% | 0.708× | 0.724× | 0.45% / 0.46% |
| `native_class/optimized/free/bound/real_time` | 13.616 | 6.450 | -52.63% | — | — | 73.90% / 0.14% |
| `native_class/optimized/free/construct/real_time` | 117.384 | 108.603 | -7.48% | — | — | 0.03% / 1.29% |
| `native_class/optimized/free/dynamic/real_time` | 117.812 | 117.389 | -0.36% | — | — | 0.08% / 0.48% |
| `native_class/optimized/free/empty/real_time` | 1.733 | 1.597 | -7.90% | 0.832× | 0.783× | 10.14% / 0.13% |
| `native_class/optimized/free/failure/real_time` | 328.135 | 321.617 | -1.99% | — | — | 0.54% / 0.18% |
| `native_class/optimized/free/floating/real_time` | 1.462 | 1.429 | -2.26% | 0.414× | 0.418× | 0.57% / 0.03% |
| `native_class/optimized/free/gc/real_time` | 1,337.268 | 1,253.195 | -6.29% | — | — | 11.16% / 1.09% |
| `native_class/optimized/free/getter/real_time` | 1.468 | 1.452 | -1.09% | 1.141× | 1.157× | 0.57% / 0.26% |
| `native_class/optimized/free/integer/real_time` | 1.629 | 1.600 | -1.77% | 1.252× | 1.286× | 1.65% / 0.70% |
| `native_class/optimized/free/nullable/real_time` | 2.436 | 2.380 | -2.27% | 1.159× | 1.178× | 0.03% / 0.16% |
| `native_class/optimized/free/reentry/real_time` | 15.493 | 15.370 | -0.79% | — | — | 0.04% / 0.32% |
| `native_class/optimized/free/repeated/real_time` | 41.393 | 41.022 | -0.89% | — | — | 0.11% / 0.07% |
| `native_class/optimized/free/setter/real_time` | 1.997 | 1.601 | -19.84% | 0.952× | 0.769× | 26.52% / 0.47% |
| `native_class/optimized/free/shared/real_time` | 141.186 | 134.169 | -4.97% | — | — | 1.95% / 1.33% |
| `native_class/optimized/free/static/real_time` | 1.302 | 1.279 | -1.75% | 0.838× | 1.019× | 0.26% / 0.24% |
| `native_class/optimized/free/unique/real_time` | 137.264 | 131.357 | -4.30% | — | — | 0.23% / 0.76% |
| `native_class/optimized/free/void/real_time` | 1.619 | 1.592 | -1.63% | 0.773× | 0.785× | 0.37% / 0.15% |
| `native_class/reload_baseline/class/alternating/real_time` | 5.313 | 5.225 | -1.66% | 2.429× | 2.473× | 0.17% / 0.07% |
| `native_class/reload_baseline/class/boolean/real_time` | 5.664 | 5.421 | -4.30% | 2.722× | 2.691× | 0.36% / 0.08% |
| `native_class/reload_baseline/class/bound/real_time` | 14.217 | 14.643 | +3.00% | — | — | 0.12% / 0.16% |
| `native_class/reload_baseline/class/construct/real_time` | 122.864 | 115.974 | -5.61% | — | — | 0.48% / 5.00% |
| `native_class/reload_baseline/class/dynamic/real_time` | 189.093 | 183.184 | -3.13% | — | — | 1.67% / 0.03% |
| `native_class/reload_baseline/class/empty/real_time` | 5.703 | 5.445 | -4.53% | 2.738× | 2.669× | 0.75% / 0.29% |
| `native_class/reload_baseline/class/failure/real_time` | 331.303 | 323.410 | -2.38% | — | — | 0.07% / 0.78% |
| `native_class/reload_baseline/class/floating/real_time` | 5.600 | 5.469 | -2.34% | 1.588× | 1.599× | 0.05% / 0.62% |
| `native_class/reload_baseline/class/gc/real_time` | 1,120.135 | 1,102.607 | -1.56% | — | — | 0.68% / 1.03% |
| `native_class/reload_baseline/class/getter/real_time` | 5.792 | 5.382 | -7.08% | 4.501× | 4.288× | 0.73% / 0.36% |
| `native_class/reload_baseline/class/integer/real_time` | 5.722 | 5.391 | -5.79% | 4.399× | 4.333× | 1.76% / 0.10% |
| `native_class/reload_baseline/class/nullable/real_time` | 7.205 | 6.961 | -3.38% | 3.428× | 3.445× | 1.75% / 0.17% |
| `native_class/reload_baseline/class/reentry/real_time` | 34.126 | 33.332 | -2.33% | — | — | 0.47% / 0.07% |
| `native_class/reload_baseline/class/repeated/real_time` | 46.894 | 46.015 | -1.87% | — | — | 0.52% / 0.62% |
| `native_class/reload_baseline/class/setter/real_time` | 5.640 | 5.417 | -3.96% | 2.688× | 2.602× | 0.03% / 0.04% |
| `native_class/reload_baseline/class/shared/real_time` | 144.344 | 138.153 | -4.29% | — | — | 5.08% / 2.66% |
| `native_class/reload_baseline/class/static/real_time` | 4.252 | 4.178 | -1.75% | 2.739× | 3.329× | 0.76% / 0.32% |
| `native_class/reload_baseline/class/unique/real_time` | 142.409 | 135.920 | -4.56% | — | — | 1.40% / 0.45% |
| `native_class/reload_baseline/class/void/real_time` | 5.702 | 5.408 | -5.15% | 2.724× | 2.665× | 2.14% / 0.05% |
| `native_class/reload_baseline/free/alternating/real_time` | 5.375 | 5.259 | -2.15% | 2.457× | 2.489× | 1.08% / 1.21% |
| `native_class/reload_baseline/free/boolean/real_time` | 7.550 | 5.365 | -28.94% | 3.628× | 2.663× | 36.71% / 0.04% |
| `native_class/reload_baseline/free/bound/real_time` | 22.215 | 18.741 | -15.64% | — | — | 24.98% / 0.17% |
| `native_class/reload_baseline/free/construct/real_time` | 122.171 | 111.900 | -8.41% | — | — | 0.57% / 1.78% |
| `native_class/reload_baseline/free/dynamic/real_time` | 117.927 | 119.073 | +0.97% | — | — | 0.32% / 0.22% |
| `native_class/reload_baseline/free/empty/real_time` | 5.578 | 5.344 | -4.20% | 2.678× | 2.620× | 0.18% / 0.63% |
| `native_class/reload_baseline/free/failure/real_time` | 329.609 | 326.367 | -0.98% | — | — | 1.10% / 0.50% |
| `native_class/reload_baseline/free/floating/real_time` | 5.530 | 5.385 | -2.61% | 1.568× | 1.574× | 0.19% / 0.20% |
| `native_class/reload_baseline/free/gc/real_time` | 1,126.259 | 1,088.023 | -3.39% | — | — | 1.26% / 0.34% |
| `native_class/reload_baseline/free/getter/real_time` | 5.535 | 5.338 | -3.57% | 4.301× | 4.253× | 0.47% / 0.64% |
| `native_class/reload_baseline/free/integer/real_time` | 5.636 | 5.309 | -5.80% | 4.333× | 4.268× | 0.71% / 0.15% |
| `native_class/reload_baseline/free/nullable/real_time` | 7.137 | 6.972 | -2.31% | 3.396× | 3.451× | 0.55% / 0.26% |
| `native_class/reload_baseline/free/reentry/real_time` | 34.904 | 33.964 | -2.69% | — | — | 5.57% / 3.15% |
| `native_class/reload_baseline/free/repeated/real_time` | 46.987 | 45.854 | -2.41% | — | — | 1.40% / 0.34% |
| `native_class/reload_baseline/free/setter/real_time` | 5.605 | 5.349 | -4.56% | 2.671× | 2.570× | 0.05% / 0.05% |
| `native_class/reload_baseline/free/shared/real_time` | 143.221 | 141.444 | -1.24% | — | — | 0.10% / 0.07% |
| `native_class/reload_baseline/free/static/real_time` | 4.249 | 4.181 | -1.61% | 2.737× | 3.332× | 0.36% / 0.13% |
| `native_class/reload_baseline/free/unique/real_time` | 141.460 | 136.050 | -3.82% | — | — | 0.25% / 0.38% |
| `native_class/reload_baseline/free/void/real_time` | 5.555 | 5.316 | -4.31% | 2.654× | 2.619× | 0.42% / 0.06% |
| `native_class/reload_optimized/class/alternating/real_time` | 5.351 | 5.312 | -0.73% | 2.446× | 2.514× | 0.82% / 1.10% |
| `native_class/reload_optimized/class/boolean/real_time` | 5.646 | 5.405 | -4.27% | 2.713× | 2.683× | 0.45% / 0.70% |
| `native_class/reload_optimized/class/bound/real_time` | 5.579 | 5.156 | -7.59% | — | — | 4.97% / 0.43% |
| `native_class/reload_optimized/class/construct/real_time` | 123.718 | 113.833 | -7.99% | — | — | 2.18% / 3.37% |
| `native_class/reload_optimized/class/dynamic/real_time` | 187.315 | 185.604 | -0.91% | — | — | 0.62% / 1.63% |
| `native_class/reload_optimized/class/empty/real_time` | 5.630 | 5.430 | -3.54% | 2.702× | 2.662× | 0.02% / 0.26% |
| `native_class/reload_optimized/class/failure/real_time` | 330.454 | 324.955 | -1.66% | — | — | 0.70% / 0.39% |
| `native_class/reload_optimized/class/floating/real_time` | 5.589 | 5.434 | -2.77% | 1.585× | 1.589× | 0.03% / 0.08% |
| `native_class/reload_optimized/class/gc/real_time` | 1,106.302 | 1,106.425 | +0.01% | — | — | 3.16% / 1.56% |
| `native_class/reload_optimized/class/getter/real_time` | 5.654 | 5.403 | -4.44% | 4.394× | 4.305× | 0.12% / 1.29% |
| `native_class/reload_optimized/class/integer/real_time` | 5.658 | 5.415 | -4.30% | 4.350× | 4.352× | 0.31% / 0.72% |
| `native_class/reload_optimized/class/nullable/real_time` | 7.194 | 7.038 | -2.16% | 3.423× | 3.483× | 0.05% / 1.25% |
| `native_class/reload_optimized/class/reentry/real_time` | 46.928 | 34.649 | -26.16% | — | — | 32.01% / 1.53% |
| `native_class/reload_optimized/class/repeated/real_time` | 46.772 | 46.231 | -1.15% | — | — | 0.30% / 0.92% |
| `native_class/reload_optimized/class/setter/real_time` | 5.635 | 5.425 | -3.74% | 2.686× | 2.606× | 0.25% / 0.12% |
| `native_class/reload_optimized/class/shared/real_time` | 143.881 | 137.361 | -4.53% | — | — | 0.27% / 0.21% |
| `native_class/reload_optimized/class/static/real_time` | 4.238 | 4.162 | -1.80% | 2.729× | 3.316× | 0.01% / 0.17% |
| `native_class/reload_optimized/class/unique/real_time` | 140.446 | 134.844 | -3.99% | — | — | 1.16% / 1.52% |
| `native_class/reload_optimized/class/void/real_time` | 5.588 | 5.415 | -3.10% | 2.670× | 2.668× | 0.05% / 0.15% |
| `native_class/reload_optimized/free/alternating/real_time` | 5.462 | 5.581 | +2.17% | 2.497× | 2.642× | 2.12% / 3.72% |
| `native_class/reload_optimized/free/boolean/real_time` | 5.579 | 5.341 | -4.28% | 2.681× | 2.651× | 0.42% / 0.86% |
| `native_class/reload_optimized/free/bound/real_time` | 10.388 | 10.444 | +0.54% | — | — | 0.73% / 0.16% |
| `native_class/reload_optimized/free/construct/real_time` | 121.897 | 112.613 | -7.62% | — | — | 0.06% / 2.81% |
| `native_class/reload_optimized/free/dynamic/real_time` | 120.495 | 120.417 | -0.06% | — | — | 1.27% / 0.21% |
| `native_class/reload_optimized/free/empty/real_time` | 5.564 | 5.337 | -4.09% | 2.671× | 2.616× | 0.76% / 0.55% |
| `native_class/reload_optimized/free/failure/real_time` | 332.157 | 324.317 | -2.36% | — | — | 0.69% / 0.65% |
| `native_class/reload_optimized/free/floating/real_time` | 5.549 | 5.378 | -3.07% | 1.573× | 1.572× | 0.69% / 0.01% |
| `native_class/reload_optimized/free/gc/real_time` | 1,119.036 | 1,084.100 | -3.12% | — | — | 1.08% / 1.38% |
| `native_class/reload_optimized/free/getter/real_time` | 5.517 | 5.338 | -3.24% | 4.287× | 4.253× | 0.07% / 0.70% |
| `native_class/reload_optimized/free/integer/real_time` | 5.545 | 5.329 | -3.89% | 4.263× | 4.284× | 0.02% / 0.13% |
| `native_class/reload_optimized/free/nullable/real_time` | 7.407 | 6.952 | -6.14% | 3.525× | 3.441× | 5.75% / 0.13% |
| `native_class/reload_optimized/free/reentry/real_time` | 33.771 | 34.097 | +0.97% | — | — | 0.18% / 0.69% |
| `native_class/reload_optimized/free/repeated/real_time` | 46.830 | 45.987 | -1.80% | — | — | 0.11% / 0.44% |
| `native_class/reload_optimized/free/setter/real_time` | 5.594 | 5.371 | -3.99% | 2.666× | 2.580× | 0.77% / 0.11% |
| `native_class/reload_optimized/free/shared/real_time` | 145.972 | 140.242 | -3.93% | — | — | 2.48% / 0.51% |
| `native_class/reload_optimized/free/static/real_time` | 4.271 | 4.331 | +1.38% | 2.751× | 3.451× | 0.05% / 5.07% |
| `native_class/reload_optimized/free/unique/real_time` | 141.822 | 136.637 | -3.66% | — | — | 0.69% / 0.19% |
| `native_class/reload_optimized/free/void/real_time` | 5.762 | 5.302 | -8.00% | 2.753× | 2.612× | 6.20% / 0.61% |
| `reflection/aot/call/cached/real_time` | 0.569 | 0.553 | -2.98% | 0.569× | 0.546× | 0.08% / 0.01% |
| `reflection/aot/call/direct/real_time` | 0.102 | 0.106 | +4.58% | 1.820× | 1.887× | 0.00% / 5.83% |
| `reflection/aot/call/dynamic/real_time` | 134.821 | 135.607 | +0.58% | — | — | 0.21% / 0.66% |
| `reflection/aot/call/member_lookup/real_time` | 1.300 | 1.268 | -2.49% | 0.653× | 0.642× | 1.82% / 0.01% |
| `reflection/aot/call/packed/real_time` | 0.861 | 0.855 | -0.73% | — | — | 0.43% / 0.02% |
| `reflection/aot/call/type_member_lookup/real_time` | 4.489 | 4.471 | -0.40% | 0.909× | 1.140× | 0.61% / 0.12% |
| `reflection/aot/construct/direct/real_time` | 17.911 | 18.116 | +1.14% | — | — | 0.07% / 0.37% |
| `reflection/aot/construct/reflected/real_time` | 91.597 | 113.575 | +24.00% | — | — | 0.24% / 26.81% |
| `reflection/aot/field/cached/real_time` | 0.846 | 0.832 | -1.59% | 0.517× | 0.508× | 1.61% / 0.17% |
| `reflection/aot/field/direct/real_time` | 0.104 | 0.120 | +15.35% | 1.863× | 2.147× | 2.83% / 22.13% |
| `reflection/aot/field/dynamic/real_time` | 42.433 | 44.524 | +4.93% | — | — | 0.17% / 0.38% |
| `reflection/aot/field/member_lookup/real_time` | 2.242 | 2.227 | -0.67% | 0.757× | 0.851× | 0.12% / 0.53% |
| `reflection/aot/field/type_member_lookup/real_time` | 4.124 | 4.126 | +0.06% | 0.915× | 0.846× | 0.11% / 0.01% |
| `reflection/aot/lookup/member/real_time` | 0.736 | 0.132 | -82.03% | — | — | 0.04% / 0.56% |
| `reflection/aot/lookup/type/real_time` | 0.690 | 0.133 | -80.67% | — | — | 0.18% / 0.02% |
| `reflection/aot/metadata/enumerate/real_time` | 245.267 | 257.770 | +5.10% | — | — | 4.71% / 0.71% |
| `reflection/aot/query/all/background:1024/matches:0/real_time` | 57.146 | 57.749 | +1.06% | — | — | 0.23% / 0.08% |
| `reflection/aot/query/all/background:1024/matches:256/real_time` | 67,380.459 | 76,245.121 | +13.16% | — | — | 0.37% / 7.19% |
| `reflection/aot/query/all/background:1024/matches:64/real_time` | 16,254.582 | 17,158.239 | +5.56% | — | — | 1.12% / 0.44% |
| `reflection/aot/query/all/background:1024/matches:8/real_time` | 1,846.061 | 1,952.673 | +5.78% | — | — | 0.34% / 0.43% |
| `reflection/aot/query/all/background:64/matches:8/real_time` | 2,260.329 | 2,286.180 | +1.14% | — | — | 0.09% / 1.13% |
| `reflection/aot/query/missing/background:1024/matches:8/real_time` | 440.188 | 451.693 | +2.61% | — | — | 13.07% / 12.03% |
| `reflection/aot/query/module/background:1024/matches:8/real_time` | 2,218.871 | 2,525.378 | +13.81% | — | — | 6.09% / 4.14% |
| `reflection/aot/unused/registered_direct/real_time` | 0.101 | 0.102 | +0.21% | — | — | 0.48% / 0.25% |
| `reflection/baseline/call/cached/real_time` | 13.399 | 13.349 | -0.38% | 13.395× | 13.190× | 0.10% / 0.22% |
| `reflection/baseline/call/direct/real_time` | 15.599 | 15.786 | +1.20% | 278.874× | 279.757× | 0.33% / 0.98% |
| `reflection/baseline/call/dynamic/real_time` | 204.863 | 205.363 | +0.24% | — | — | 2.18% / 0.01% |
| `reflection/baseline/call/member_lookup/real_time` | 12.215 | 12.312 | +0.79% | 6.138× | 6.239× | 0.08% / 0.45% |
| `reflection/baseline/call/packed/real_time` | 14.721 | 9.855 | -33.06% | — | — | 2.04% / 0.20% |
| `reflection/baseline/call/type_member_lookup/real_time` | 13.501 | 13.476 | -0.18% | 2.733× | 3.436× | 0.20% / 0.10% |
| `reflection/baseline/construct/direct/real_time` | 45.858 | 46.218 | +0.78% | — | — | 0.20% / 1.02% |
| `reflection/baseline/construct/reflected/real_time` | 141.538 | 143.087 | +1.09% | — | — | 1.07% / 0.50% |
| `reflection/baseline/field/cached/real_time` | 7.362 | 7.408 | +0.62% | 4.503× | 4.524× | 0.20% / 1.33% |
| `reflection/baseline/field/direct/real_time` | 3.981 | 3.934 | -1.18% | 71.039× | 70.125× | 1.47% / 0.33% |
| `reflection/baseline/field/dynamic/real_time` | 62.896 | 63.100 | +0.33% | — | — | 0.79% / 0.13% |
| `reflection/baseline/field/member_lookup/real_time` | 10.339 | 10.457 | +1.14% | 3.492× | 3.997× | 0.23% / 0.11% |
| `reflection/baseline/field/type_member_lookup/real_time` | 11.779 | 11.797 | +0.15% | 2.613× | 2.419× | 0.39% / 0.30% |
| `reflection/baseline/lookup/member/real_time` | 2.949 | 2.988 | +1.32% | — | — | 0.42% / 0.98% |
| `reflection/baseline/lookup/type/real_time` | 2.624 | 2.622 | -0.04% | — | — | 0.59% / 0.20% |
| `reflection/baseline/metadata/enumerate/real_time` | 283.373 | 278.495 | -1.72% | — | — | 5.10% / 0.33% |
| `reflection/baseline/query/all/background:1024/matches:0/real_time` | 63.578 | 63.714 | +0.21% | — | — | 0.09% / 0.69% |
| `reflection/baseline/query/all/background:1024/matches:256/real_time` | 72,440.716 | 79,888.111 | +10.28% | — | — | 0.85% / 0.50% |
| `reflection/baseline/query/all/background:1024/matches:64/real_time` | 17,299.511 | 18,037.678 | +4.27% | — | — | 0.03% / 0.51% |
| `reflection/baseline/query/all/background:1024/matches:8/real_time` | 2,114.845 | 2,134.445 | +0.93% | — | — | 0.24% / 0.95% |
| `reflection/baseline/query/all/background:64/matches:8/real_time` | 2,660.295 | 3,251.516 | +22.22% | — | — | 0.21% / 26.01% |
| `reflection/baseline/query/missing/background:1024/matches:8/real_time` | 596.863 | 469.416 | -21.35% | — | — | 22.03% / 11.25% |
| `reflection/baseline/query/module/background:1024/matches:8/real_time` | 3,064.350 | 2,445.466 | -20.20% | — | — | 27.40% / 2.51% |
| `reflection/baseline/unused/registered_direct/real_time` | 3.955 | 4.668 | +18.03% | — | — | 0.92% / 23.14% |
| `reflection/closed/call/cached/real_time` | 5.883 | 5.843 | -0.69% | 5.881× | 5.773× | 0.87% / 0.07% |
| `reflection/closed/call/direct/real_time` | 8.258 | 8.095 | -1.98% | 147.640× | 143.461× | 1.35% / 0.17% |
| `reflection/closed/call/dynamic/real_time` | 135.768 | 136.243 | +0.35% | — | — | 0.31% / 0.51% |
| `reflection/closed/call/member_lookup/real_time` | 8.173 | 6.504 | -20.41% | 4.107× | 3.296× | 30.49% / 1.06% |
| `reflection/closed/call/packed/real_time` | 5.595 | 4.789 | -14.40% | — | — | 21.80% / 0.11% |
| `reflection/closed/call/type_member_lookup/real_time` | 8.105 | 8.124 | +0.24% | 1.641× | 2.071× | 0.33% / 0.24% |
| `reflection/closed/construct/direct/real_time` | 18.632 | 19.140 | +2.73% | — | — | 0.31% / 3.35% |
| `reflection/closed/construct/reflected/real_time` | 100.754 | 101.166 | +0.41% | — | — | 0.76% / 0.28% |
| `reflection/closed/field/cached/real_time` | 5.014 | 5.083 | +1.37% | 3.067× | 3.104× | 0.28% / 1.35% |
| `reflection/closed/field/direct/real_time` | 0.909 | 0.914 | +0.58% | 16.227× | 16.302× | 0.26% / 1.36% |
| `reflection/closed/field/dynamic/real_time` | 42.941 | 45.954 | +7.02% | — | — | 0.64% / 0.01% |
| `reflection/closed/field/member_lookup/real_time` | 7.088 | 7.067 | -0.30% | 2.394× | 2.701× | 0.57% / 0.09% |
| `reflection/closed/field/type_member_lookup/real_time` | 8.805 | 8.836 | +0.34% | 1.953× | 1.811× | 1.26% / 1.47% |
| `reflection/closed/lookup/member/real_time` | 2.049 | 2.039 | -0.46% | — | — | 1.46% / 0.11% |
| `reflection/closed/lookup/type/real_time` | 0.707 | 0.710 | +0.51% | — | — | 1.22% / 0.02% |
| `reflection/closed/metadata/enumerate/real_time` | 297.097 | 302.138 | +1.70% | — | — | 0.21% / 1.30% |
| `reflection/closed/query/all/background:1024/matches:0/real_time` | 62.052 | 62.048 | -0.01% | — | — | 0.33% / 0.20% |
| `reflection/closed/query/all/background:1024/matches:256/real_time` | 67,198.896 | 76,102.374 | +13.25% | — | — | 0.56% / 0.42% |
| `reflection/closed/query/all/background:1024/matches:64/real_time` | 16,986.177 | 18,355.450 | +8.06% | — | — | 1.99% / 0.30% |
| `reflection/closed/query/all/background:1024/matches:8/real_time` | 2,102.325 | 2,248.106 | +6.93% | — | — | 0.13% / 0.79% |
| `reflection/closed/query/all/background:64/matches:8/real_time` | 2,835.796 | 2,824.644 | -0.39% | — | — | 0.28% / 0.82% |
| `reflection/closed/query/missing/background:1024/matches:8/real_time` | 433.896 | 546.258 | +25.90% | — | — | 0.91% / 28.65% |
| `reflection/closed/query/module/background:1024/matches:8/real_time` | 2,351.300 | 2,593.084 | +10.28% | — | — | 0.78% / 4.17% |
| `reflection/closed/unused/registered_direct/real_time` | 0.906 | 0.911 | +0.59% | — | — | 0.44% / 0.64% |
| `reflection/cpp/call/cached/real_time` | 1.000 | 1.012 | +1.17% | 1.000× | 1.000× | 0.30% / 1.05% |
| `reflection/cpp/call/direct/real_time` | 0.056 | 0.056 | +0.88% | 1.000× | 1.000× | 0.19% / 1.43% |
| `reflection/cpp/call/member_lookup/real_time` | 1.990 | 1.973 | -0.85% | 1.000× | 1.000× | 1.09% / 0.11% |
| `reflection/cpp/call/type_member_lookup/real_time` | 4.939 | 3.923 | -20.58% | 1.000× | 1.000× | 29.35% / 0.55% |
| `reflection/cpp/field/cached/real_time` | 1.635 | 1.637 | +0.15% | 1.000× | 1.000× | 0.24% / 0.04% |
| `reflection/cpp/field/direct/real_time` | 0.056 | 0.056 | +0.11% | 1.000× | 1.000× | 0.27% / 0.26% |
| `reflection/cpp/field/member_lookup/real_time` | 2.961 | 2.616 | -11.64% | 1.000× | 1.000× | 17.28% / 0.57% |
| `reflection/cpp/field/type_member_lookup/real_time` | 4.509 | 4.878 | +8.19% | 1.000× | 1.000× | 0.09% / 10.00% |
| `reflection/optimized/call/cached/real_time` | 9.335 | 9.380 | +0.48% | 9.332× | 9.269× | 0.08% / 0.01% |
| `reflection/optimized/call/direct/real_time` | 15.485 | 15.714 | +1.48% | 276.849× | 278.494× | 0.24% / 0.11% |
| `reflection/optimized/call/dynamic/real_time` | 204.391 | 203.286 | -0.54% | — | — | 0.94% / 0.10% |
| `reflection/optimized/call/member_lookup/real_time` | 8.262 | 8.334 | +0.87% | 4.151× | 4.223× | 0.05% / 0.05% |
| `reflection/optimized/call/packed/real_time` | 5.799 | 5.777 | -0.38% | — | — | 0.01% / 0.35% |
| `reflection/optimized/call/type_member_lookup/real_time` | 9.761 | 9.745 | -0.16% | 1.976× | 2.484× | 0.39% / 0.34% |
| `reflection/optimized/construct/direct/real_time` | 52.157 | 52.567 | +0.78% | — | — | 0.08% / 0.30% |
| `reflection/optimized/construct/reflected/real_time` | 151.605 | 154.840 | +2.13% | — | — | 0.24% / 0.52% |
| `reflection/optimized/field/cached/real_time` | 8.977 | 7.374 | -17.86% | 5.492× | 4.504× | 26.08% / 0.26% |
| `reflection/optimized/field/direct/real_time` | 0.678 | 0.692 | +2.03% | 12.102× | 12.335× | 0.78% / 2.76% |
| `reflection/optimized/field/dynamic/real_time` | 62.965 | 63.419 | +0.72% | — | — | 0.53% / 0.87% |
| `reflection/optimized/field/member_lookup/real_time` | 10.329 | 10.429 | +0.96% | 3.488× | 3.986× | 0.12% / 0.33% |
| `reflection/optimized/field/type_member_lookup/real_time` | 11.754 | 11.789 | +0.30% | 2.607× | 2.417× | 0.10% / 0.49% |
| `reflection/optimized/lookup/member/real_time` | 2.947 | 2.989 | +1.41% | — | — | 0.03% / 1.17% |
| `reflection/optimized/lookup/type/real_time` | 2.617 | 2.632 | +0.56% | — | — | 0.05% / 0.88% |
| `reflection/optimized/metadata/enumerate/real_time` | 293.197 | 277.128 | -5.48% | — | — | 1.84% / 0.08% |
| `reflection/optimized/query/all/background:1024/matches:0/real_time` | 63.727 | 63.751 | +0.04% | — | — | 0.22% / 0.41% |
| `reflection/optimized/query/all/background:1024/matches:256/real_time` | 72,098.916 | 80,672.036 | +11.89% | — | — | 0.33% / 0.56% |
| `reflection/optimized/query/all/background:1024/matches:64/real_time` | 17,298.705 | 18,138.253 | +4.85% | — | — | 0.13% / 0.18% |
| `reflection/optimized/query/all/background:1024/matches:8/real_time` | 2,484.882 | 2,182.052 | -12.19% | — | — | 21.48% / 1.88% |
| `reflection/optimized/query/all/background:64/matches:8/real_time` | 2,646.857 | 2,685.295 | +1.45% | — | — | 0.20% / 0.19% |
| `reflection/optimized/query/missing/background:1024/matches:8/real_time` | 429.778 | 467.332 | +8.74% | — | — | 0.27% / 11.50% |
| `reflection/optimized/query/module/background:1024/matches:8/real_time` | 2,514.401 | 2,555.333 | +1.63% | — | — | 3.60% / 1.20% |
| `reflection/optimized/unused/registered_direct/real_time` | 0.676 | 0.675 | -0.11% | — | — | 0.16% / 0.00% |
| `reflection/reload/resources/iterations:1/real_time` | 13,395,083.344 | 13,536,570.313 | +1.06% | — | — | 0.38% / 0.64% |

## 构建与复现

源码基线为 `ab2b9fb6b7c161a5e84895e6421ce80b92bbc7c0`，包含 JSON 中记录的未提交修改。
公共项目链接输入冻结 396 个；没有创建 Git 工作树或切换共享 SDK。
测量开始：2026-11-23T00:51:59.735552+08:00；结束：2026-11-23T01:15:26.256619+08:00。

各组使用相同参数：

```sh
/path/to/frozen/vela_aot_benchmarks --benchmark_filter="^(aot/(native|cpp)/|hot/steady/)" \
  --benchmark_min_time=0.2s --benchmark_repetitions=2 \
  --benchmark_enable_random_interleaving=true --benchmark_out_format=json \
  --benchmark_out=/tmp/vela-comparison.json
```

完整二进制路径、SHA-256、各组 filter 和执行命令均保存在滚动摘要中。两侧按组交替执行，基准之间顺序运行。
原始样本、日志、冻结对象和一次性脚本保留在系统临时目录；仓库只保存聚合结果。

实现与测量边界见 [基准说明](../../benchmarks/README.md)、[反射](reflection.md)、
[C++ 互操作](cpp_interop.md)及 [GC](garbage_collection.md)。

`after/native_class_compare` 初次输出不完整，已整组重新测量两次并通过完整性检查；未将不完整输出与重测数据拼接。
