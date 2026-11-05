# Vela 与 C++ 互操作基准报告

测量时间：2026-11-05 11:27:59 至 2026-11-05 11:37:59，北京时间（UTC+8）。

本报告覆盖当前 C++ 互操作实现的全部现有宿主类基准，以及标量绑定和类型化宿主 API 基准：**279 项，每项 7 次，共 1953 个有效重复样本**。所有预期用例、校验和与用例自身的资源检查通过。这是互操作微基准报告；算法吞吐、编译时间、热重载事务耗时和多线程调度不在本次测量范围。

对象创建组的 30 项场景在完整 GC 后仍各有 **24 KiB（24,576 B）记账存量增量**；宿主对象构造、析构数量配对，共享所有者增量为 0。此保留量在资源节单独列明，不能将整套结果概括为“全部零保留”。

4096 次批量下，整数宿主调用的中位数为 C++ 直接调用 **2.099 ns/次**、Vela JIT 调用 C++ **3.543 ns/次**，时间比为 **1.688×**，绝对差为 **1.444 ns/次**。表中的倍数均表示“被测路径耗时 ÷ 对照路径耗时”，越低越好。

整数宿主方法的 C++ 指针对照为 **1.292 ns/次**，闭合 Optimized JIT 类绑定为 **2.140 ns/次**（1.656×），可重载 Optimized 为 **9.044 ns/次**（7.000×）。可重载路径的额外成本在当前数据中明显可见。

另一方向的完整类型化入口 `C++ → Vela → C++` 为 **120.483 ns/次**，对应低层受查运行时入口为 **103.928 ns/次**，包装层差值 **16.556 ns/次**。它与批量脚本循环中的单次宿主调用具有不同的计时边界。

本轮有 **15 / 279 项 CV 超过 20%**，相关时间以 `†` 标记。这些结果只能支持当前机器上的描述性比较，微小差异不足以证明提升或退化。完整统计见附录，不能把接近 1 的比值解释为已经证明“零开销”。

## 1. 环境与证据

| 项目 | 记录 |
| --- | ---: |
| CPU / 架构 | Apple M1 Pro / arm64，8 个逻辑 CPU |
| 操作系统 | Darwin 27.0.0 |
| 宿主编译器 | Fuchsia clang version 23.0.0git (https://llvm.googlesource.com/llvm-project 80743bd43fd5b38fedc503308e7a652e23d3ec93) |
| LLVM / Clang tooling SDK | 21.1.8 / 21.1.8 |
| 宿主构建 | `Release; -O3 -DNDEBUG -std=c++23 -fno-exceptions` |
| Vela 代码优化 | JIT / AOT 使用 LLVM O2；Baseline 与 Optimized 是 Vela 代码生成层级，均非 O0 |
| Google Benchmark | 1.9.5 |
| 源码提交 | `8c99f4e67e51eb8249bcacd2886d1a9726e7992c`，包含未提交修改；完整状态与文件 SHA-256 见摘要 |
| 编译器构建身份 | `fe092c79b34ae9593f06cdc24e2e98004d80ab811d7b114194dae0455e5b5bbb/release` |
| 结果摘要 SHA-256 | `c16da25ad0a2ed315aca3757000b09d4974f3dbeb8e63079357905cc6f8688b2` |

机器可读证据为[滚动摘要](../../benchmarks/results/rolling_summary.json)的 `latest`。`previous` 保留上一轮 109 项验收数据；本轮扩大为 279 项，因此未给出整套前后加速比。本报告的 C++ 比较均从同一轮结果中配对计算。原始重复样本不作为仓库历史保存。

| 套件 | 项目数 | 覆盖 |
| --- | ---: | ---: |
| `native` | 32 | 4 种工作量 × 2 种批量 × C++ 直接 / C++ 间接 / JIT 宿主 / JIT control |
| `native_class` | 210 | 19 种工作量 × 5 种模式 × 类绑定 / 自由函数，加 20 项 C++ 指针 / 手写适配器 |
| `interop` | 37 | JIT / AOT 类型化与运行时入口、AOT 生成包装、内建值、类型解析、Future 完成 |

## 2. 对照口径与统计方法

全部时间使用 wall time，批量项目以每批耗时除以 `calls_per_iteration`，得到 ns/次；非批量项目报告 ns/操作。每项重复 7 次，最短测量时间为 0.2 秒，随机交错执行。中位数和范围来自 7 次重复的平均每操作耗时；σ 为这些重复的样本标准差，CV = σ / 重复均值。它们不是单次调用延迟的 P50、P95 或 P99。

编译、注册、输入准备和预热不计入稳态时间，常规 GC 计入；最终资源清理与完整 GC 不计入。每批包含循环、累加、结果检查及摊销后的宿主入口成本；未从总耗时中扣掉估算项。本机未固定线程亲和性，CPU 频率元数据不可用；wall time 不依赖该频率字段。结果仅适用于此 arm64 机器和所列构建配置。

| 路径 | 实际测量内容 | 可回答的问题 |
| --- | ---: | ---: |
| `native/cpp` | C++ 循环直接调用独立翻译单元中的禁止内联内核 | 相同业务内核的原生直接调用成本 |
| `native/cpp_indirect` | C++ 循环通过目标被隐藏的函数指针调用同一内核 | 原生间接调用成本 |
| `native/jit` | Vela 脚本循环调用同一 C++ 内核，携带 NativeContext | 脚本到宿主的批量稳态成本 |
| `native/jit_control` | Vela 循环内直接完成对应表达式，不调用宿主 | 观察循环和生成代码差异；不用于宣称精确 ABI 净开销 |
| `native_class/cpp_pointer` | C++ 循环经函数指针传入裸对象指针，再调用禁止内联方法 | 同一宿主方法的 C++ 指针对照；属于间接调用 |
| `native_class/cpp_adapter` | C++ 循环经函数指针传入对象引用的手写适配器 | 同一宿主方法的手写 C++ 适配对照；属于间接调用 |
| `native_class/*/free` | Vela 调用 C++ 自由函数，自由函数委托给相同宿主方法 | 类绑定相对等价自由函数绑定的差异 |
| `interop/*/runtime` | C++ 使用 Vela 低层受查运行时完成相同操作 | 类型化 API 包装的增量成本 |

`free` 与 `runtime` 都仍使用 Vela 运行时。容器、字符串、对象、持久根与 Future 在本轮没有 `std::vector`、`std::string` 或纯 C++ 调度器的等价实现，因此没有给它们标注“相对纯 C++”倍数。

## 3. 标量及字符串调用：与直接、间接 C++ 比较

`empty` 保留空调用边界；`integer` 计算 `value + 1`；`double` 计算 `value + 0.5`；`string_borrow` 对固定的 `vela-native` 取长度及首字节之和。内核接收 `NativeContext&`，这些内核不使用句柄。JIT 使用默认 Baseline 层级。全部时间单位为 ns/次。

| 工作量 | 每批次数 | C++ 直接 | C++ 间接 | Vela JIT → C++ | JIT / 直接 | JIT / 间接 | JIT − 直接 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| empty | 64 | 1.606 | 1.606 | 3.945 | 2.457× | 2.456× | +2.339 |
| empty | 4096 | 2.436 | 2.487 | 3.566 | 1.464× | 1.434× | +1.130 |
| integer | 64 | 1.437 | 1.407 | 3.963 | 2.758× | 2.816× | +2.526 |
| integer | 4096 | 2.099 | 2.153 | 3.543 | 1.688× | 1.645× | +1.444 |
| double | 64 | 3.541 | 3.547 | 4.877 | 1.377× | 1.375× | +1.336 |
| double | 4096 | 3.533 | 3.540 | 4.513 | 1.278× | 1.275× | +0.981 |
| string_borrow | 64 | 1.072 | 1.035 | 13.259 | 12.373× | 12.816× | +12.188 |
| string_borrow | 4096 | 0.967 | 0.970 | 12.537 | 12.964× | 12.921× | +11.570 |

64 次批量更容易暴露每次进入脚本的固定成本；4096 次批量更接近热循环内的摊销成本。两种批量都保留，不能以大批量结果代替应用中的单次入口延迟。

| 工作量 | 每批次数 | JIT control | JIT 宿主调用 | 独立中位数之差 |
| --- | ---: | ---: | ---: | ---: |
| empty | 64 | 0.820 | 3.945 | +3.126 |
| empty | 4096 | 0.345 | 3.566 | +3.222 |
| integer | 64 | 0.813 | 3.963 | +3.149 |
| integer | 4096 | 0.339 | 3.543 | +3.203 |
| double | 64 | 1.271 | 4.877 | +3.606 |
| double | 4096 | 0.971 | 4.513 | +3.542 |
| string_borrow | 64 | 0.474 | 13.259 | +12.785 |
| string_borrow | 4096 | 0.007 | 12.537 | +12.529 |

control 可能被编译器整体化简，字符串 control 直接使用已知结果。两列之差来自独立重复的中位数，包含生成代码变化，不是隔离后的单次跨语言 ABI 成本。

## 4. 宿主类：与 C++ 指针及手写适配器比较

这一组比较相同 `NativeClassHost` 方法。`cpp_pointer` 与 `cpp_adapter` 都通过被隐藏目标的函数指针调用，不能与第 3 节的直接 C++ 基线混称。表中每种 Vela 模式显示“ns/次（相对 C++ pointer 的时间比）”。这些项目每批均为 4096 次；脚本在每批开始重置接收者状态，其固定成本包含在结果中。

| 工作量 | C++ pointer | C++ adapter | AOT | JIT Baseline | JIT Optimized | Reload Baseline | Reload Optimized |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| empty | 2.075 | 2.096 † | 2.335（1.125×） | 2.270（1.094×） | 2.277（1.097×） | 9.164（4.417×） | 9.306（4.485×） |
| integer | 1.292 | 1.289 | 2.198（1.701×） | 2.233（1.728×） | 2.140（1.656×） | 9.120（7.058×） | 9.044（7.000×） |
| alternating | 1.305 | 1.303 | 2.576（1.974×） | 2.645（2.026×） | 2.575（1.973×） | 9.704 †（7.436×） | 9.754（7.474×） |
| floating | 3.549 | 3.532 | 2.176（0.613×） | 2.219（0.625×） | 2.194（0.618×） | 9.143（2.576×） | 9.102（2.565×） |
| boolean | 2.128 | 2.091 | 2.554（1.200×） | 2.607（1.225×） | 2.581（1.213×） | 9.611（4.517×） | 9.401（4.418×） |
| nullable | 2.103 | 2.123 | 4.563（2.169×） | 4.240 †（2.016×） | 4.224（2.008×） | 12.477（5.932×） | 12.403（5.897×） |
| void | 2.100 | 2.125 | 2.152（1.025×） | 2.121（1.010×） | 2.150（1.024×） | 9.299（4.428×） | 9.263（4.411×） |
| getter | 1.300 | 1.291 | 2.182（1.679×） | 2.203（1.694×） | 2.133（1.640×） | 9.163（7.048×） | 9.038（6.952×） |
| setter | 2.125 | 2.142 | 1.940（0.913×） | 1.957（0.921×） | 1.949（0.917×） | 9.190（4.324×） | 9.235（4.345×） |
| static | 1.301 | 1.291 | 1.110（0.853×） | 1.115（0.857×） | 1.138（0.875×） | 4.276（3.286×） | 4.281（3.291×） |

`alternating` 交替使用两个接收者；`nullable` 交替传入空指针和有效对象；`void`、`getter`、`setter`、`static` 分别覆盖无返回值、属性读写和静态方法。其余 9 种场景没有对应的原生 C++ 基准，完整数据在下一节列出；不以不同语义的整数调用充当对照。

## 5. 全部宿主类路径：类绑定与自由函数绑定

下列 5 张表覆盖 190 项 Vela 路径，单位为 ns/次，比例为 class / free。`aot` 为离线代码；`baseline` / `optimized` 为闭合 JIT；`reload_baseline` / `reload_optimized` 为可重载发布中的稳态调用。可重载项目没有在计时循环中执行源码变更或重载事务。

`bound` 每批创建一次绑定方法或等价闭包后调用 4096 次；`dynamic` 使用动态分派；`construct`、`unique`、`shared` 每批创建 64 个对象并读取属性；`repeated` 重复返回同一个共享对象；`failure` 每批捕获 16 次宿主错误；`gc` 每批显式请求 16 次 GC；`reentry` 每批完成 64 次脚本 → C++ → 脚本往返。异常与重入的 ns/次包含整条操作链。

### aot

| 工作量 | 每批次数 | 类绑定 | 自由函数绑定 | class / free | 差值 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 4,096 | 2.335 | 2.308 | 1.012× | +0.027 |
| integer | 4,096 | 2.198 | 2.179 | 1.009× | +0.019 |
| alternating | 4,096 | 2.576 | 2.592 | 0.994× | -0.016 |
| floating | 4,096 | 2.176 | 2.174 | 1.001× | +0.002 |
| boolean | 4,096 | 2.554 | 2.545 | 1.003× | +0.009 |
| nullable | 4,096 | 4.563 | 4.215 | 1.082× | +0.348 |
| void | 4,096 | 2.152 | 2.199 | 0.979× | -0.047 |
| getter | 4,096 | 2.182 | 2.140 | 1.020× | +0.042 |
| setter | 4,096 | 1.940 | 1.974 | 0.983× | -0.034 |
| static | 4,096 | 1.110 | 1.119 | 0.993× | -0.008 |
| bound | 4,096 | 2.014 | 11.454 | 0.176× | -9.440 |
| dynamic | 4,096 | 296.802 | 251.722 | 1.179× | +45.081 |
| construct | 64 | 127.416 | 127.624 | 0.998× | -0.207 |
| unique | 64 | 120.671 | 122.654 | 0.984× | -1.982 |
| shared | 64 | 125.298 | 124.059 | 1.010× | +1.239 |
| repeated | 4,096 | 49.015 | 49.004 | 1.000× | +0.011 |
| failure | 16 | 399.064 | 395.139 | 1.010× | +3.925 |
| gc | 16 | 1168.988 | 1165.525 | 1.003× | +3.463 |
| reentry | 64 | 79.873 | 80.490 | 0.992× | -0.617 |

### baseline

| 工作量 | 每批次数 | 类绑定 | 自由函数绑定 | class / free | 差值 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 4,096 | 2.270 | 2.286 | 0.993× | -0.016 |
| integer | 4,096 | 2.233 | 2.204 | 1.014× | +0.030 |
| alternating | 4,096 | 2.645 | 2.600 | 1.017× | +0.045 |
| floating | 4,096 | 2.219 | 2.196 | 1.011× | +0.023 |
| boolean | 4,096 | 2.607 | 2.577 | 1.011× | +0.029 |
| nullable | 4,096 | 4.240 † | 4.286 | 0.989× | -0.046 |
| void | 4,096 | 2.121 | 2.186 | 0.970× | -0.065 |
| getter | 4,096 | 2.203 | 2.193 | 1.004× | +0.010 |
| setter | 4,096 | 1.957 | 1.954 | 1.002× | +0.003 |
| static | 4,096 | 1.115 | 1.121 † | 0.995× | -0.005 |
| bound | 4,096 | 39.280 | 36.974 | 1.062× | +2.306 |
| dynamic | 4,096 | 292.147 | 255.566 | 1.143× | +36.580 |
| construct | 64 | 132.168 | 127.638 | 1.035× | +4.530 |
| unique | 64 | 120.668 | 121.608 | 0.992× | -0.940 |
| shared | 64 | 126.277 | 121.966 | 1.035× | +4.311 |
| repeated | 4,096 | 49.832 † | 48.974 | 1.018× | +0.858 |
| failure | 16 | 400.782 | 407.104 | 0.984× | -6.322 |
| gc | 16 | 1167.278 † | 1186.276 | 0.984× | -18.998 |
| reentry | 64 | 80.378 | 82.839 | 0.970× | -2.461 |

### optimized

| 工作量 | 每批次数 | 类绑定 | 自由函数绑定 | class / free | 差值 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 4,096 | 2.277 | 2.332 | 0.976× | -0.055 |
| integer | 4,096 | 2.140 | 2.127 † | 1.006× | +0.013 |
| alternating | 4,096 | 2.575 | 2.582 | 0.997× | -0.006 |
| floating | 4,096 | 2.194 | 2.181 | 1.006× | +0.013 |
| boolean | 4,096 | 2.581 | 2.605 | 0.991× | -0.024 |
| nullable | 4,096 | 4.224 | 4.176 † | 1.011× | +0.048 |
| void | 4,096 | 2.150 | 2.175 | 0.989× | -0.025 |
| getter | 4,096 | 2.133 | 2.155 | 0.989× | -0.023 |
| setter | 4,096 | 1.949 | 2.022 | 0.964× | -0.072 |
| static | 4,096 | 1.138 | 1.125 | 1.012× | +0.013 |
| bound | 4,096 | 1.510 | 11.005 | 0.137× | -9.495 |
| dynamic | 4,096 | 296.261 | 254.078 | 1.166× | +42.183 |
| construct | 64 | 128.984 | 127.382 | 1.013× | +1.602 |
| unique | 64 | 118.299 | 120.908 | 0.978× | -2.609 |
| shared | 64 | 122.349 | 139.796 | 0.875× | -17.447 |
| repeated | 4,096 | 49.296 | 49.387 | 0.998× | -0.091 |
| failure | 16 | 400.515 | 397.440 | 1.008× | +3.075 |
| gc | 16 | 1160.896 | 1170.921 † | 0.991× | -10.024 |
| reentry | 64 | 81.647 | 80.527 | 1.014× | +1.121 |

### reload_baseline

| 工作量 | 每批次数 | 类绑定 | 自由函数绑定 | class / free | 差值 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 4,096 | 9.164 | 19.822 | 0.462× | -10.658 |
| integer | 4,096 | 9.120 | 9.024 | 1.011× | +0.096 |
| alternating | 4,096 | 9.704 † | 9.970 | 0.973× | -0.266 |
| floating | 4,096 | 9.143 | 9.039 | 1.011× | +0.104 |
| boolean | 4,096 | 9.611 | 9.469 | 1.015× | +0.142 |
| nullable | 4,096 | 12.477 | 12.439 | 1.003× | +0.037 |
| void | 4,096 | 9.299 | 9.165 | 1.015× | +0.134 |
| getter | 4,096 | 9.163 | 9.299 † | 0.985× | -0.136 |
| setter | 4,096 | 9.190 | 9.042 | 1.016× | +0.149 |
| static | 4,096 | 4.276 | 4.262 | 1.003× | +0.013 |
| bound | 4,096 | 26.544 | 33.716 | 0.787× | -7.173 |
| dynamic | 4,096 | 379.029 | 242.014 | 1.566× | +137.016 |
| construct | 64 | 137.291 | 138.143 | 0.994× | -0.853 |
| unique | 64 | 129.369 | 130.137 | 0.994× | -0.769 |
| shared | 64 | 130.033 | 131.350 | 0.990× | -1.317 |
| repeated | 4,096 | 56.005 | 55.852 | 1.003× | +0.153 |
| failure | 16 | 399.360 | 398.286 | 1.003× | +1.074 |
| gc | 16 | 1059.477 | 1071.328 | 0.989× | -11.851 |
| reentry | 64 | 115.173 | 113.533 | 1.014× | +1.640 |

### reload_optimized

| 工作量 | 每批次数 | 类绑定 | 自由函数绑定 | class / free | 差值 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 4,096 | 9.306 | 19.763 | 0.471× | -10.457 |
| integer | 4,096 | 9.044 | 9.013 | 1.003× | +0.031 |
| alternating | 4,096 | 9.754 | 9.477 | 1.029× | +0.277 |
| floating | 4,096 | 9.102 | 9.327 | 0.976× | -0.225 |
| boolean | 4,096 | 9.401 | 9.448 | 0.995× | -0.047 |
| nullable | 4,096 | 12.403 | 12.453 | 0.996× | -0.050 |
| void | 4,096 | 9.263 | 9.416 | 0.984× | -0.153 |
| getter | 4,096 | 9.038 | 9.004 | 1.004× | +0.034 |
| setter | 4,096 | 9.235 | 9.295 | 0.994× | -0.060 |
| static | 4,096 | 4.281 | 4.277 | 1.001× | +0.004 |
| bound | 4,096 | 8.818 | 19.912 | 0.443× | -11.094 |
| dynamic | 4,096 | 374.645 | 238.106 | 1.573× | +136.539 |
| construct | 64 | 138.779 | 137.198 † | 1.012× | +1.581 |
| unique | 64 | 132.231 | 129.623 | 1.020× | +2.607 |
| shared | 64 | 130.932 | 131.819 | 0.993× | -0.887 |
| repeated | 4,096 | 56.559 | 55.987 | 1.010× | +0.572 |
| failure | 16 | 402.761 | 398.601 | 1.010× | +4.160 |
| gc | 16 | 1052.452 | 1046.144 | 1.006× | +6.308 |
| reentry | 64 | 113.231 | 115.605 | 0.979× | -2.373 |

## 6. 类型化 C++ API：与低层 Vela 运行时比较

这一节测量公共类型、句柄及入口包装的增量成本。所有基线都使用 Vela 的值和生命周期机制；时间单位为 ns/操作。JIT 使用默认 Baseline 层级。各组的准备阶段相同，操作期间都包含临时 `HandleScope` 的创建与销毁。

`scalar` 为 C++ → 脚本 → C++ `step`；`local` 为把已建数组传给脚本，经过宿主 echo 返回，检查身份并读取元素；`callback` 调用预先创建的脚本闭包；`object` 读取已创建对象的公开字段，不包含对象构造；`reentry` 每次创建函数值并经过脚本和宿主往返。

| 场景 | 低层 runtime | 类型化 typed | typed / runtime | 差值 ns |
| --- | ---: | ---: | ---: | ---: |
| jit/scalar | 103.928 | 120.483 † | 1.159× | +16.556 |
| jit/local | 161.850 | 188.241 | 1.163× | +26.391 |
| jit/callback | 132.857 † | 144.484 | 1.088× | +11.627 |
| jit/object | 98.327 | 113.904 † | 1.158× | +15.577 |
| jit/reentry | 335.746 | 358.802 | 1.069× | +23.056 |
| aot/scalar | 114.849 | 121.906 | 1.061× | +7.057 |
| aot/local | 170.024 | 193.069 | 1.136× | +23.045 |
| aot/callback | 136.990 | 150.145 | 1.096× | +13.155 |
| aot/object | 99.371 | 103.069 | 1.037× | +3.699 |
| aot/reentry | 342.584 | 363.580 † | 1.061× | +20.996 |

### AOT 生成包装

生成包装使用公共生成接口完成完整调用；它与预先绑定的 `Entry` 具有不同的入口工作量。

| 场景 | 低层 runtime | 生成 wrapper | wrapper / runtime | 差值 ns |
| --- | ---: | ---: | ---: | ---: |
| scalar | 114.849 | 147.130 | 1.281× | +32.281 |
| local | 170.024 | 224.140 | 1.318× | +54.116 |
| reentry | 342.584 | 393.460 | 1.149× | +50.875 |

### 内建值、持久根及 Future

字符串输入为 42 字节，数组读取索引 0 的预建元素。persistent 测量持久根的创建和释放；completion 包含创建完成能力、同线程完成 Future、读取结果及释放，不测跨线程投递延迟。

| 场景 | 低层 runtime | 类型化 typed | typed / runtime | 差值 ns |
| --- | ---: | ---: | ---: | ---: |
| values/string_borrow | 11.329 | 10.948 | 0.966× | -0.380 |
| values/string_copy | 38.973 | 39.531 | 1.014× | +0.558 |
| values/array_scalar | 11.638 | 21.061 | 1.810× | +9.423 |
| values/array_reference | 14.943 | 24.568 | 1.644× | +9.625 |
| values/persistent | 48.127 | 54.600 | 1.135× | +6.473 |
| async/completion | 888.332 | 944.728 | 1.063× | +56.396 |

### 冷类型绑定与已绑定类型创建

| 场景 | ns/操作 | 实际计时范围 |
| --- | ---: | ---: |
| `type/cold` | 1561.948 | 新 Isolate 上绑定类型、创建空数组、销毁局部作用域和类型；Isolate 创建、显式 GC、关闭除外 |
| `type/bound` | 40.061 | 在已绑定类型上创建空数组并销毁局部作用域 |

两项工作量不同，不能把它们相除当成同一操作的缓存加速比；本次也没有单独隔离 `Type::Bind` 的纯耗时。

## 7. 分配、根与完整回收

Vela 分配统计覆盖 Heap 及其记账的原生存储、持久根，不覆盖全部进程 malloc、`std::string` 副本或元数据中的 STL 分配。`scope_storage_bytes` 为 `sizeof(HandleScope)`，本机为 336 字节；它不是实际栈峰值，也不表示每个标量宿主调用都会构造一个作用域。

external roots 和 shutdown blockers 在计时循环外采样，表示准备好的资源数量；blockers 还包含执行与调度资源。它们不是调用期间的峰值。保留量检查先结束已有增量周期，再按当前根启动完整回收。

### 类型化 API 的全部资源记录

分配次数、分配字节均按每次操作归一化；峰值增量是该次重复测量期间的 Heap 高水位中位数，没有按操作数归一化。baseline 与 full GC 是存量快照；cold 不导出这两个快照，其清理由内部断言检查。

| 场景（省略 interop/） | 分配/操作 | 字节/操作 | 准备根 | 准备 blockers | baseline B | full GC B | 保留增量 B | 峰值增量 B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| aot/runtime/callback | 0 | 0 | 3 | 3 | 472 | 472 | 0 | 0 |
| aot/runtime/local | 0 | 0 | 3 | 3 | 536 | 536 | 0 | 0 |
| aot/runtime/object | 0 | 0 | 4 | 4 | 752 | 752 | 0 | 0 |
| aot/runtime/reentry | 1 | 64 | 2 | 2 | 408 | 408 | 0 | 65,088 |
| aot/runtime/scalar | 0 | 0 | 2 | 2 | 408 | 408 | 0 | 0 |
| aot/typed/callback | 0 | 0 | 3 | 3 | 472 | 472 | 0 | 0 |
| aot/typed/local | 0 | 0 | 3 | 3 | 536 | 536 | 0 | 0 |
| aot/typed/object | 0 | 0 | 4 | 4 | 752 | 752 | 0 | 0 |
| aot/typed/reentry | 1 | 64 | 2 | 2 | 408 | 408 | 0 | 65,088 |
| aot/typed/scalar | 0 | 0 | 2 | 2 | 408 | 408 | 0 | 0 |
| aot/wrapper/local | 0 | 0 | 3 | 3 | 536 | 536 | 0 | 0 |
| aot/wrapper/reentry | 1 | 64 | 2 | 2 | 408 | 408 | 0 | 65,088 |
| aot/wrapper/scalar | 0 | 0 | 2 | 2 | 408 | 408 | 0 | 0 |
| async/runtime/completion | 2 | 256 | 1 | 1 | 216 | 216 | 0 | 65,280 |
| async/typed/completion | 2 | 256 | 1 | 1 | 216 | 216 | 0 | 65,280 |
| jit/runtime/callback | 0 | 0 | 3 | 3 | 472 | 472 | 0 | 0 |
| jit/runtime/local | 0 | 0 | 3 | 3 | 536 | 536 | 0 | 0 |
| jit/runtime/object | 0 | 0 | 4 | 4 | 752 | 752 | 0 | 0 |
| jit/runtime/reentry | 1 | 64 | 2 | 2 | 408 | 408 | 0 | 65,088 |
| jit/runtime/scalar | 0 | 0 | 2 | 2 | 408 | 408 | 0 | 0 |
| jit/typed/callback | 0 | 0 | 3 | 3 | 472 | 472 | 0 | 0 |
| jit/typed/local | 0 | 0 | 3 | 3 | 536 | 536 | 0 | 0 |
| jit/typed/object | 0 | 0 | 4 | 4 | 752 | 752 | 0 | 0 |
| jit/typed/reentry | 1 | 64 | 2 | 2 | 408 | 408 | 0 | 65,088 |
| jit/typed/scalar | 0 | 0 | 2 | 2 | 408 | 408 | 0 | 0 |
| type/bound | 1 | 128 | 1 | 1 | 216 | 216 | 0 | 65,280 |
| type/cold | 2 | 344 | 0 | 0 | 未采集 | 未采集 | 0 | 344 |
| values/runtime/array_reference | 0 | 0 | 1 | 1 | 256 | 256 | 0 | 0 |
| values/runtime/array_scalar | 0 | 0 | 1 | 1 | 256 | 256 | 0 | 0 |
| values/runtime/persistent | 1 | 96 | 1 | 1 | 64 | 64 | 0 | 96 |
| values/runtime/string_borrow | 0 | 0 | 1 | 1 | 128 | 128 | 0 | 0 |
| values/runtime/string_copy | 0 | 0 | 1 | 1 | 128 | 128 | 0 | 0 |
| values/typed/array_reference | 0 | 0 | 1 | 1 | 256 | 256 | 0 | 0 |
| values/typed/array_scalar | 0 | 0 | 1 | 1 | 256 | 256 | 0 | 0 |
| values/typed/persistent | 1 | 96 | 1 | 1 | 64 | 64 | 0 | 96 |
| values/typed/string_borrow | 0 | 0 | 1 | 1 | 128 | 128 | 0 | 0 |
| values/typed/string_copy | 0 | 0 | 1 | 1 | 128 | 128 | 0 | 0 |

scalar、local、object、callback、数组读取和字符串借用的两种路径没有 Vela 记账分配；字符串复制的 `std::string` 分配不在此统计范围。persistent 每操作 1 次、96 B；Future completion 每操作 2 次、256 B；reentry 每操作创建函数值，1 次、64 B；cold 为 2 次、344 B；bound 为数组本身的 1 次、128 B。typed 与 runtime 的分配计数相同，全部 37 项清理后额外保留字节为 0。

例如 baseline 408 B、full GC 408 B，含义是清理后 Vela 记账存量恢复到基线。即便 reentry 每次分配 64 B，也可以得到 408 B → 408 B；这不表示没有临时分配，也不表示进程 RSS 为 408 B。

### 宿主类资源

下表给出同一工作量在 5 种模式、class/free 两条路径中的每调用分配次数范围。构造和销毁量来自宿主计数器，销毁量在完整 GC 后采样；峰值是每次重复的 Vela 记账存量高水位。范围取各路径的 7 次重复中位数之间的最小值与最大值。所有 190 项共享所有者增量均为 0。20 项原生 C++ 基线没有分配计数器，不能写作“已测得 0 分配”。

| 工作量 | 分配/调用 | 构造/调用 | 清理后销毁/调用 | 峰值存量 B（跨路径范围） | 完整 GC 后保留增量 B |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 0 | 0 | 0 | 1,072–1,280 | 0 |
| integer | 0 | 0 | 0 | 1,072–1,280 | 0 |
| alternating | 0 | 0 | 0 | 1,224–1,432 | 0 |
| floating | 0 | 0 | 0 | 1,072–1,280 | 0 |
| boolean | 0 | 0 | 0 | 1,072–1,280 | 0 |
| nullable | 0 | 0 | 0 | 1,072–1,280 | 0 |
| void | 0 | 0 | 0 | 1,072–1,280 | 0 |
| getter | 0 | 0 | 0 | 1,072–1,280 | 0 |
| setter | 0 | 0 | 0 | 1,072–1,280 | 0 |
| static | 0 | 0 | 0 | 1,072–1,280 | 0 |
| bound | 0.000244 | 0 | 0 | 65,520–65,584 | 0 |
| dynamic | 2.000244–5 | 0 | 0 | 65,488–65,600 | 0 |
| construct | 2.000001–2.000020 | 1 | 1 | 86,528–99,112 | 24,576 |
| unique | 2.000001–2.000007 | 1 | 1 | 86,528–99,928 | 24,576 |
| shared | 2.000003–2.000015 | 1 | 1 | 97,136–100,232 | 24,576 |
| repeated | 0 | 0 | 0 | 1,224–1,432 | 0 |
| failure | 4 | 0 | 0 | 65,456–65,536 | 0 |
| gc | 0 | 0 | 0 | 1,072–1,280 | 0 |
| reentry | 0 | 0 | 0 | 1,072–1,280 | 0 |

bound 每批分配 1 次，在 4096 次调用上摊销为约 0.000244 次/调用；dynamic 则实际存在每调用 2–5 次分配，部分路径另有每批一次的准备成本。`construct` / `unique` / `shared` 还涉及 C++ 对象自身的 new 或共享控制块；Vela 分配计数不等于所有宿主 malloc 次数。

`construct`、`unique`、`shared` 的全部 30 项场景，在每次重复中都观察到 24,576 B 的最终保留增量，同时宿主构造与析构数量完全配对。其余 160 项脚本场景保留增量为 0。[HostIndex](../../runtime/heap/host_index.cc) 使用 Heap 记账的原生内存，并且仅在索引为空时释放容量；基准始终保留全局接收者。这个机制允许非空索引的容量在回收后保留，但当前计数没有拆分这 24 KiB 的具体组成，因此不将其直接断言为对象泄漏或已经完成归因。

### 标量调用组的 Heap 存量

native 组只导出 Heap 起止存量与对象数，没有累计分配计数。下列所有快照都来自同一批量任务，存量相等不能推导出期间分配次数为 0。

| 场景（省略 native/） | 起始 B | 结束 B | 起始对象 | 结束对象 |
| --- | ---: | ---: | ---: | ---: |
| jit/empty/64 | 192 | 192 | 3 | 3 |
| jit_control/empty/64 | 192 | 192 | 3 | 3 |
| jit/empty/4096 | 192 | 192 | 3 | 3 |
| jit_control/empty/4096 | 192 | 192 | 3 | 3 |
| jit/integer/64 | 192 | 192 | 3 | 3 |
| jit_control/integer/64 | 192 | 192 | 3 | 3 |
| jit/integer/4096 | 192 | 192 | 3 | 3 |
| jit_control/integer/4096 | 192 | 192 | 3 | 3 |
| jit/double/64 | 192 | 192 | 3 | 3 |
| jit_control/double/64 | 192 | 192 | 3 | 3 |
| jit/double/4096 | 192 | 192 | 3 | 3 |
| jit_control/double/4096 | 192 | 192 | 3 | 3 |
| jit/string_borrow/64 | 336 | 336 | 4 | 4 |
| jit_control/string_borrow/64 | 192 | 192 | 3 | 3 |
| jit/string_borrow/4096 | 336 | 336 | 4 | 4 |
| jit_control/string_borrow/4096 | 192 | 192 | 3 | 3 |

## 8. 复现与解释边界

在仓库根目录先通过 `python3 -m tools.vl build release benchmarks` 构建 Release。下面为本轮三个已构建二进制的实际测量命令；输出位于系统临时目录。同样的 native 和 native_class 测量也可使用 `vl bench --native-compare` 与 `vl bench --native-class-compare`，两者默认使用本报告的 7 次、0.2 秒和随机交错设置。

```sh
/Users/zoy/vela/out/release/vela_aot_benchmarks '--benchmark_filter=^native_class/' --benchmark_min_time=0.2s --benchmark_repetitions=7 --benchmark_enable_random_interleaving=true --benchmark_out_format=json --benchmark_out=/tmp/vela-cpp-report-native_class.json
/Users/zoy/vela/out/release/vela_aot_benchmarks '--benchmark_filter=^interop/' --benchmark_min_time=0.2s --benchmark_repetitions=7 --benchmark_enable_random_interleaving=true --benchmark_out_format=json --benchmark_out=/tmp/vela-cpp-report-interop.json
/Users/zoy/vela/out/release/vela_benchmarks '--benchmark_filter=^native/' --benchmark_min_time=0.2s --benchmark_repetitions=7 --benchmark_enable_random_interleaving=true --benchmark_out_format=json --benchmark_out=/tmp/vela-cpp-report-native.json
```

本报告未测量单次调用的尾延迟、进程 RSS、全进程分配、单独冷启动、热重载事务、跨线程 Future 投递或通用算法吞吐。没有为缺少等价工作量的项目构造倍数，也没有将低层 runtime 或 Vela 自由函数路径解释为脱离运行时的原生 C++。

实现依据：标量内核与计时见 [native_call_benchmark.cc](../../benchmarks/native_call_benchmark.cc)、[native_call_kernels.cc](../../benchmarks/native_call_kernels.cc)；类工作量和 C++ 对照见 [native_class_benchmark.cc](../../benchmarks/native_class_benchmark.cc)、[native_class_host.cc](../../benchmarks/native_class_host.cc)、[工作量生成器](../../tools/native_class_benchmarks.py)；公共 API 计时见 [host_interop_benchmark.cc](../../benchmarks/host_interop_benchmark.cc)；完整回收口径见 [heap_metrics.cc](../../benchmarks/heap_metrics.cc)。类型与生命周期契约见 [C++ 互操作设计](cpp_interop.md)。

## 附录：279 项完整时间统计

每行均为 7 次有效重复。中位数、σ、最小值和最大值均已换算为 ns/次或 ns/操作；N 为每批调用数，N = 1 的 interop 项为每次完整操作。CV 大于 20% 的中位数后标记 `†`。

### native：32 项

| 用例 | N | 中位数 ns | σ ns | CV | 最小 ns | 最大 ns |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `native/cpp/double/4096/real_time` | 4,096 | 3.533 | 0.023 | 0.66% | 3.518 | 3.587 |
| `native/cpp/double/64/real_time` | 64 | 3.541 | 0.037 | 1.05% | 3.519 | 3.615 |
| `native/cpp/empty/4096/real_time` | 4,096 | 2.436 | 0.163 | 6.53% | 2.423 | 2.864 |
| `native/cpp/empty/64/real_time` | 64 | 1.606 | 0.014 | 0.86% | 1.594 | 1.635 |
| `native/cpp/integer/4096/real_time` | 4,096 | 2.099 | 0.015 | 0.72% | 2.090 | 2.128 |
| `native/cpp/integer/64/real_time` | 64 | 1.437 | 0.278 | 18.13% | 1.407 | 2.163 |
| `native/cpp/string_borrow/4096/real_time` | 4,096 | 0.967 | 0.016 | 1.67% | 0.962 | 1.005 |
| `native/cpp/string_borrow/64/real_time` | 64 | 1.072 | 0.007 | 0.61% | 1.069 | 1.084 |
| `native/cpp_indirect/double/4096/real_time` | 4,096 | 3.540 | 0.093 | 2.59% | 3.529 | 3.774 |
| `native/cpp_indirect/double/64/real_time` | 64 | 3.547 | 0.033 | 0.93% | 3.534 | 3.610 |
| `native/cpp_indirect/empty/4096/real_time` | 4,096 | 2.487 | 0.108 | 4.29% | 2.451 | 2.757 |
| `native/cpp_indirect/empty/64/real_time` | 64 | 1.606 | 0.067 | 4.14% | 1.591 | 1.779 |
| `native/cpp_indirect/integer/4096/real_time` | 4,096 | 2.153 | 0.021 | 0.98% | 2.130 | 2.184 |
| `native/cpp_indirect/integer/64/real_time` | 64 | 1.407 | 0.040 | 2.81% | 1.399 | 1.513 |
| `native/cpp_indirect/string_borrow/4096/real_time` | 4,096 | 0.970 | 0.026 | 2.67% | 0.960 | 1.037 |
| `native/cpp_indirect/string_borrow/64/real_time` | 64 | 1.035 | 0.010 | 1.01% | 1.028 | 1.058 |
| `native/jit/double/4096/real_time` | 4,096 | 4.513 | 0.049 | 1.08% | 4.504 | 4.643 |
| `native/jit/double/64/real_time` | 64 | 4.877 | 0.022 | 0.45% | 4.839 | 4.900 |
| `native/jit/empty/4096/real_time` | 4,096 | 3.566 | 0.118 | 3.26% | 3.536 | 3.865 |
| `native/jit/empty/64/real_time` | 64 | 3.945 | 0.053 | 1.33% | 3.893 | 4.053 |
| `native/jit/integer/4096/real_time` | 4,096 | 3.543 | 0.011 | 0.32% | 3.534 | 3.566 |
| `native/jit/integer/64/real_time` | 64 | 3.963 | 0.034 | 0.84% | 3.928 | 4.023 |
| `native/jit/string_borrow/4096/real_time` | 4,096 | 12.537 | 0.113 | 0.90% | 12.503 | 12.796 |
| `native/jit/string_borrow/64/real_time` | 64 | 13.259 | 0.126 | 0.95% | 13.163 | 13.480 |
| `native/jit_control/double/4096/real_time` | 4,096 | 0.971 | 0.014 | 1.42% | 0.966 | 1.004 |
| `native/jit_control/double/64/real_time` | 64 | 1.271 | 0.238 | 17.30% | 1.269 | 1.910 |
| `native/jit_control/empty/4096/real_time` | 4,096 | 0.345 | 0.021 | 6.10% | 0.336 | 0.397 |
| `native/jit_control/empty/64/real_time` | 64 | 0.820 | 0.009 | 1.09% | 0.813 | 0.837 |
| `native/jit_control/integer/4096/real_time` | 4,096 | 0.339 | 0.004 | 1.25% | 0.335 | 0.346 |
| `native/jit_control/integer/64/real_time` | 64 | 0.813 | 0.006 | 0.69% | 0.806 | 0.821 |
| `native/jit_control/string_borrow/4096/real_time` | 4,096 | 0.007 | 0.000 | 1.36% | 0.007 | 0.008 |
| `native/jit_control/string_borrow/64/real_time` | 64 | 0.474 | 0.018 | 3.65% | 0.471 | 0.520 |

### native_class：210 项

| 用例 | N | 中位数 ns | σ ns | CV | 最小 ns | 最大 ns |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `native_class/aot/class/alternating/real_time` | 4,096 | 2.576 | 0.057 | 2.21% | 2.560 | 2.719 |
| `native_class/aot/class/boolean/real_time` | 4,096 | 2.554 | 0.084 | 3.26% | 2.516 | 2.754 |
| `native_class/aot/class/bound/real_time` | 4,096 | 2.014 | 0.019 | 0.95% | 2.011 | 2.065 |
| `native_class/aot/class/construct/real_time` | 64 | 127.416 | 5.071 | 3.96% | 121.652 | 135.155 |
| `native_class/aot/class/dynamic/real_time` | 4,096 | 296.802 | 47.072 | 14.90% | 287.896 | 419.044 |
| `native_class/aot/class/empty/real_time` | 4,096 | 2.335 | 0.159 | 6.63% | 2.262 | 2.718 |
| `native_class/aot/class/failure/real_time` | 16 | 399.064 | 31.733 | 7.72% | 396.185 | 482.792 |
| `native_class/aot/class/floating/real_time` | 4,096 | 2.176 | 0.082 | 3.70% | 2.144 | 2.384 |
| `native_class/aot/class/gc/real_time` | 16 | 1168.988 | 72.287 | 6.04% | 1134.500 | 1301.865 |
| `native_class/aot/class/getter/real_time` | 4,096 | 2.182 | 0.076 | 3.49% | 2.093 | 2.315 |
| `native_class/aot/class/integer/real_time` | 4,096 | 2.198 | 0.089 | 4.04% | 2.114 | 2.373 |
| `native_class/aot/class/nullable/real_time` | 4,096 | 4.563 | 0.147 | 3.19% | 4.483 | 4.877 |
| `native_class/aot/class/reentry/real_time` | 64 | 79.873 | 5.355 | 6.45% | 79.466 | 94.019 |
| `native_class/aot/class/repeated/real_time` | 4,096 | 49.015 | 0.305 | 0.62% | 48.725 | 49.569 |
| `native_class/aot/class/setter/real_time` | 4,096 | 1.940 | 0.150 | 7.54% | 1.914 | 2.330 |
| `native_class/aot/class/shared/real_time` | 64 | 125.298 | 4.760 | 3.83% | 118.156 | 132.612 |
| `native_class/aot/class/static/real_time` | 4,096 | 1.110 | 0.194 | 16.33% | 1.105 | 1.628 |
| `native_class/aot/class/unique/real_time` | 64 | 120.671 | 9.786 | 7.91% | 112.912 | 138.858 |
| `native_class/aot/class/void/real_time` | 4,096 | 2.152 | 0.051 | 2.36% | 2.106 | 2.252 |
| `native_class/aot/free/alternating/real_time` | 4,096 | 2.592 | 0.086 | 3.24% | 2.571 | 2.788 |
| `native_class/aot/free/boolean/real_time` | 4,096 | 2.545 | 0.317 | 11.75% | 2.505 | 3.357 |
| `native_class/aot/free/bound/real_time` | 4,096 | 11.454 | 0.190 | 1.66% | 11.189 | 11.716 |
| `native_class/aot/free/construct/real_time` | 64 | 127.624 | 9.194 | 7.05% | 121.724 | 150.390 |
| `native_class/aot/free/dynamic/real_time` | 4,096 | 251.722 | 5.433 | 2.14% | 250.614 | 265.856 |
| `native_class/aot/free/empty/real_time` | 4,096 | 2.308 | 0.367 | 14.88% | 2.286 | 3.290 |
| `native_class/aot/free/failure/real_time` | 16 | 395.139 | 4.223 | 1.06% | 392.572 | 405.081 |
| `native_class/aot/free/floating/real_time` | 4,096 | 2.174 | 0.046 | 2.09% | 2.131 | 2.272 |
| `native_class/aot/free/gc/real_time` | 16 | 1165.525 | 47.295 | 4.02% | 1131.128 | 1264.106 |
| `native_class/aot/free/getter/real_time` | 4,096 | 2.140 | 0.067 | 3.11% | 2.102 | 2.264 |
| `native_class/aot/free/integer/real_time` | 4,096 | 2.179 | 0.094 | 4.27% | 2.096 | 2.376 |
| `native_class/aot/free/nullable/real_time` | 4,096 | 4.215 | 0.169 | 3.97% | 4.157 | 4.635 |
| `native_class/aot/free/reentry/real_time` | 64 | 80.490 | 1.532 | 1.89% | 79.294 | 83.693 |
| `native_class/aot/free/repeated/real_time` | 4,096 | 49.004 | 6.970 | 13.48% | 48.838 | 67.489 |
| `native_class/aot/free/setter/real_time` | 4,096 | 1.974 | 0.049 | 2.47% | 1.908 | 2.035 |
| `native_class/aot/free/shared/real_time` | 64 | 124.059 | 1.936 | 1.57% | 120.493 | 125.180 |
| `native_class/aot/free/static/real_time` | 4,096 | 1.119 | 0.033 | 2.89% | 1.105 | 1.197 |
| `native_class/aot/free/unique/real_time` | 64 | 122.654 | 6.320 | 5.09% | 117.118 | 136.008 |
| `native_class/aot/free/void/real_time` | 4,096 | 2.199 | 0.458 | 19.11% | 2.105 | 3.366 |
| `native_class/baseline/class/alternating/real_time` | 4,096 | 2.645 | 0.099 | 3.73% | 2.562 | 2.853 |
| `native_class/baseline/class/boolean/real_time` | 4,096 | 2.607 | 0.113 | 4.26% | 2.566 | 2.857 |
| `native_class/baseline/class/bound/real_time` | 4,096 | 39.280 | 1.492 | 3.77% | 38.287 | 42.446 |
| `native_class/baseline/class/construct/real_time` | 64 | 132.168 | 7.083 | 5.36% | 123.173 | 143.588 |
| `native_class/baseline/class/dynamic/real_time` | 4,096 | 292.147 | 34.412 | 10.98% | 290.154 | 366.205 |
| `native_class/baseline/class/empty/real_time` | 4,096 | 2.270 | 0.022 | 0.95% | 2.263 | 2.314 |
| `native_class/baseline/class/failure/real_time` | 16 | 400.782 | 4.898 | 1.23% | 393.854 | 407.804 |
| `native_class/baseline/class/floating/real_time` | 4,096 | 2.219 | 0.079 | 3.54% | 2.144 | 2.365 |
| `native_class/baseline/class/gc/real_time` | 16 | 1167.278 † | 543.804 | 39.30% | 1145.017 | 2615.167 |
| `native_class/baseline/class/getter/real_time` | 4,096 | 2.203 | 0.035 | 1.59% | 2.141 | 2.255 |
| `native_class/baseline/class/integer/real_time` | 4,096 | 2.233 | 0.120 | 5.40% | 2.104 | 2.419 |
| `native_class/baseline/class/nullable/real_time` | 4,096 | 4.240 † | 1.489 | 31.06% | 4.159 | 8.170 |
| `native_class/baseline/class/reentry/real_time` | 64 | 80.378 | 1.414 | 1.75% | 79.289 | 83.142 |
| `native_class/baseline/class/repeated/real_time` | 4,096 | 49.832 † | 11.443 | 21.18% | 48.836 | 79.939 |
| `native_class/baseline/class/setter/real_time` | 4,096 | 1.957 | 0.162 | 7.99% | 1.906 | 2.303 |
| `native_class/baseline/class/shared/real_time` | 64 | 126.277 | 2.704 | 2.14% | 121.743 | 129.145 |
| `native_class/baseline/class/static/real_time` | 4,096 | 1.115 | 0.103 | 8.92% | 1.109 | 1.391 |
| `native_class/baseline/class/unique/real_time` | 64 | 120.668 | 5.651 | 4.56% | 118.582 | 133.958 |
| `native_class/baseline/class/void/real_time` | 4,096 | 2.121 | 0.111 | 5.08% | 2.114 | 2.420 |
| `native_class/baseline/free/alternating/real_time` | 4,096 | 2.600 | 0.044 | 1.69% | 2.555 | 2.688 |
| `native_class/baseline/free/boolean/real_time` | 4,096 | 2.577 | 0.032 | 1.24% | 2.567 | 2.644 |
| `native_class/baseline/free/bound/real_time` | 4,096 | 36.974 | 1.041 | 2.77% | 36.887 | 39.701 |
| `native_class/baseline/free/construct/real_time` | 64 | 127.638 | 3.310 | 2.59% | 120.952 | 131.166 |
| `native_class/baseline/free/dynamic/real_time` | 4,096 | 255.566 | 16.175 | 6.18% | 252.176 | 297.022 |
| `native_class/baseline/free/empty/real_time` | 4,096 | 2.286 | 0.163 | 6.86% | 2.255 | 2.717 |
| `native_class/baseline/free/failure/real_time` | 16 | 407.104 | 20.893 | 5.05% | 395.498 | 454.738 |
| `native_class/baseline/free/floating/real_time` | 4,096 | 2.196 | 0.047 | 2.14% | 2.170 | 2.300 |
| `native_class/baseline/free/gc/real_time` | 16 | 1186.276 | 123.893 | 10.16% | 1124.840 | 1492.239 |
| `native_class/baseline/free/getter/real_time` | 4,096 | 2.193 | 0.041 | 1.88% | 2.104 | 2.229 |
| `native_class/baseline/free/integer/real_time` | 4,096 | 2.204 | 0.043 | 1.97% | 2.116 | 2.242 |
| `native_class/baseline/free/nullable/real_time` | 4,096 | 4.286 | 0.540 | 12.11% | 4.175 | 5.677 |
| `native_class/baseline/free/reentry/real_time` | 64 | 82.839 | 5.936 | 6.99% | 79.144 | 95.508 |
| `native_class/baseline/free/repeated/real_time` | 4,096 | 48.974 | 1.647 | 3.32% | 48.806 | 53.331 |
| `native_class/baseline/free/setter/real_time` | 4,096 | 1.954 | 0.036 | 1.87% | 1.903 | 1.990 |
| `native_class/baseline/free/shared/real_time` | 64 | 121.966 | 2.663 | 2.21% | 116.410 | 123.414 |
| `native_class/baseline/free/static/real_time` | 4,096 | 1.121 † | 0.493 | 37.47% | 1.111 | 2.432 |
| `native_class/baseline/free/unique/real_time` | 64 | 121.608 | 18.000 | 13.78% | 118.064 | 168.718 |
| `native_class/baseline/free/void/real_time` | 4,096 | 2.186 | 0.069 | 3.14% | 2.126 | 2.327 |
| `native_class/cpp_adapter/alternating/real_time` | 4,096 | 1.303 | 0.067 | 5.10% | 1.284 | 1.471 |
| `native_class/cpp_adapter/boolean/real_time` | 4,096 | 2.091 | 0.014 | 0.65% | 2.080 | 2.122 |
| `native_class/cpp_adapter/empty/real_time` | 4,096 | 2.096 † | 0.470 | 20.52% | 2.076 | 3.350 |
| `native_class/cpp_adapter/floating/real_time` | 4,096 | 3.532 | 0.057 | 1.60% | 3.520 | 3.670 |
| `native_class/cpp_adapter/getter/real_time` | 4,096 | 1.291 | 0.262 | 18.43% | 1.280 | 1.986 |
| `native_class/cpp_adapter/integer/real_time` | 4,096 | 1.289 | 0.154 | 11.34% | 1.280 | 1.697 |
| `native_class/cpp_adapter/nullable/real_time` | 4,096 | 2.123 | 0.134 | 6.18% | 2.090 | 2.474 |
| `native_class/cpp_adapter/setter/real_time` | 4,096 | 2.142 | 0.064 | 2.95% | 2.115 | 2.303 |
| `native_class/cpp_adapter/static/real_time` | 4,096 | 1.291 | 0.009 | 0.66% | 1.277 | 1.298 |
| `native_class/cpp_adapter/void/real_time` | 4,096 | 2.125 | 0.045 | 2.12% | 2.090 | 2.215 |
| `native_class/cpp_pointer/alternating/real_time` | 4,096 | 1.305 | 0.010 | 0.77% | 1.283 | 1.316 |
| `native_class/cpp_pointer/boolean/real_time` | 4,096 | 2.128 | 0.153 | 7.02% | 2.068 | 2.496 |
| `native_class/cpp_pointer/empty/real_time` | 4,096 | 2.075 | 0.163 | 7.53% | 2.062 | 2.477 |
| `native_class/cpp_pointer/floating/real_time` | 4,096 | 3.549 | 0.032 | 0.90% | 3.525 | 3.614 |
| `native_class/cpp_pointer/getter/real_time` | 4,096 | 1.300 | 0.109 | 7.95% | 1.283 | 1.575 |
| `native_class/cpp_pointer/integer/real_time` | 4,096 | 1.292 | 0.057 | 4.32% | 1.284 | 1.426 |
| `native_class/cpp_pointer/nullable/real_time` | 4,096 | 2.103 | 0.059 | 2.77% | 2.082 | 2.253 |
| `native_class/cpp_pointer/setter/real_time` | 4,096 | 2.125 | 0.097 | 4.46% | 2.087 | 2.335 |
| `native_class/cpp_pointer/static/real_time` | 4,096 | 1.301 | 0.053 | 4.00% | 1.279 | 1.430 |
| `native_class/cpp_pointer/void/real_time` | 4,096 | 2.100 | 0.300 | 13.57% | 2.086 | 2.892 |
| `native_class/optimized/class/alternating/real_time` | 4,096 | 2.575 | 0.034 | 1.31% | 2.561 | 2.642 |
| `native_class/optimized/class/boolean/real_time` | 4,096 | 2.581 | 0.022 | 0.84% | 2.569 | 2.629 |
| `native_class/optimized/class/bound/real_time` | 4,096 | 1.510 | 0.046 | 3.07% | 1.462 | 1.579 |
| `native_class/optimized/class/construct/real_time` | 64 | 128.984 | 6.653 | 5.04% | 127.501 | 146.544 |
| `native_class/optimized/class/dynamic/real_time` | 4,096 | 296.261 | 20.434 | 6.72% | 292.173 | 349.071 |
| `native_class/optimized/class/empty/real_time` | 4,096 | 2.277 | 0.180 | 7.62% | 2.258 | 2.759 |
| `native_class/optimized/class/failure/real_time` | 16 | 400.515 | 39.835 | 9.47% | 392.522 | 488.972 |
| `native_class/optimized/class/floating/real_time` | 4,096 | 2.194 | 0.074 | 3.37% | 2.133 | 2.361 |
| `native_class/optimized/class/gc/real_time` | 16 | 1160.896 | 51.441 | 4.40% | 1129.279 | 1278.349 |
| `native_class/optimized/class/getter/real_time` | 4,096 | 2.133 | 0.047 | 2.21% | 2.101 | 2.240 |
| `native_class/optimized/class/integer/real_time` | 4,096 | 2.140 | 0.058 | 2.67% | 2.108 | 2.277 |
| `native_class/optimized/class/nullable/real_time` | 4,096 | 4.224 | 0.172 | 3.99% | 4.168 | 4.583 |
| `native_class/optimized/class/reentry/real_time` | 64 | 81.647 | 4.357 | 5.21% | 79.739 | 89.622 |
| `native_class/optimized/class/repeated/real_time` | 4,096 | 49.296 | 0.544 | 1.10% | 48.947 | 50.442 |
| `native_class/optimized/class/setter/real_time` | 4,096 | 1.949 | 0.014 | 0.73% | 1.924 | 1.969 |
| `native_class/optimized/class/shared/real_time` | 64 | 122.349 | 21.795 | 16.81% | 117.791 | 178.661 |
| `native_class/optimized/class/static/real_time` | 4,096 | 1.138 | 0.039 | 3.38% | 1.113 | 1.222 |
| `native_class/optimized/class/unique/real_time` | 64 | 118.299 | 2.337 | 1.98% | 114.349 | 121.181 |
| `native_class/optimized/class/void/real_time` | 4,096 | 2.150 | 0.044 | 2.03% | 2.113 | 2.243 |
| `native_class/optimized/free/alternating/real_time` | 4,096 | 2.582 | 0.023 | 0.90% | 2.560 | 2.621 |
| `native_class/optimized/free/boolean/real_time` | 4,096 | 2.605 | 0.043 | 1.64% | 2.570 | 2.671 |
| `native_class/optimized/free/bound/real_time` | 4,096 | 11.005 | 0.773 | 6.83% | 10.709 | 12.947 |
| `native_class/optimized/free/construct/real_time` | 64 | 127.382 | 1.803 | 1.42% | 123.502 | 128.747 |
| `native_class/optimized/free/dynamic/real_time` | 4,096 | 254.078 | 50.378 | 18.38% | 251.383 | 387.800 |
| `native_class/optimized/free/empty/real_time` | 4,096 | 2.332 | 0.091 | 3.86% | 2.246 | 2.467 |
| `native_class/optimized/free/failure/real_time` | 16 | 397.440 | 16.049 | 3.99% | 392.806 | 438.733 |
| `native_class/optimized/free/floating/real_time` | 4,096 | 2.181 | 0.056 | 2.55% | 2.133 | 2.293 |
| `native_class/optimized/free/gc/real_time` | 16 | 1170.921 † | 263.999 | 20.51% | 1148.962 | 1871.252 |
| `native_class/optimized/free/getter/real_time` | 4,096 | 2.155 | 0.023 | 1.06% | 2.114 | 2.182 |
| `native_class/optimized/free/integer/real_time` | 4,096 | 2.127 † | 0.954 | 38.22% | 2.103 | 4.659 |
| `native_class/optimized/free/nullable/real_time` | 4,096 | 4.176 † | 1.154 | 25.02% | 4.166 | 7.228 |
| `native_class/optimized/free/reentry/real_time` | 64 | 80.527 | 12.882 | 15.00% | 79.141 | 114.481 |
| `native_class/optimized/free/repeated/real_time` | 4,096 | 49.387 | 1.828 | 3.63% | 48.892 | 53.231 |
| `native_class/optimized/free/setter/real_time` | 4,096 | 2.022 | 0.347 | 15.95% | 1.923 | 2.895 |
| `native_class/optimized/free/shared/real_time` | 64 | 139.796 | 25.341 | 17.50% | 121.208 | 192.365 |
| `native_class/optimized/free/static/real_time` | 4,096 | 1.125 | 0.070 | 6.12% | 1.108 | 1.307 |
| `native_class/optimized/free/unique/real_time` | 64 | 120.908 | 19.849 | 15.39% | 114.090 | 171.197 |
| `native_class/optimized/free/void/real_time` | 4,096 | 2.175 | 0.081 | 3.70% | 2.113 | 2.353 |
| `native_class/reload_baseline/class/alternating/real_time` | 4,096 | 9.704 † | 2.599 | 24.28% | 9.633 | 16.594 |
| `native_class/reload_baseline/class/boolean/real_time` | 4,096 | 9.611 | 1.933 | 18.42% | 9.371 | 14.738 |
| `native_class/reload_baseline/class/bound/real_time` | 4,096 | 26.544 | 1.029 | 3.82% | 26.367 | 29.229 |
| `native_class/reload_baseline/class/construct/real_time` | 64 | 137.291 | 5.089 | 3.68% | 134.607 | 149.613 |
| `native_class/reload_baseline/class/dynamic/real_time` | 4,096 | 379.029 | 24.567 | 6.31% | 373.378 | 440.754 |
| `native_class/reload_baseline/class/empty/real_time` | 4,096 | 9.164 | 0.085 | 0.93% | 9.072 | 9.340 |
| `native_class/reload_baseline/class/failure/real_time` | 16 | 399.360 | 19.972 | 4.85% | 396.054 | 441.629 |
| `native_class/reload_baseline/class/floating/real_time` | 4,096 | 9.143 | 1.134 | 11.89% | 8.984 | 12.099 |
| `native_class/reload_baseline/class/gc/real_time` | 16 | 1059.477 | 73.336 | 6.75% | 1050.703 | 1251.641 |
| `native_class/reload_baseline/class/getter/real_time` | 4,096 | 9.163 | 0.804 | 8.37% | 8.984 | 11.104 |
| `native_class/reload_baseline/class/integer/real_time` | 4,096 | 9.120 | 0.171 | 1.87% | 9.003 | 9.498 |
| `native_class/reload_baseline/class/nullable/real_time` | 4,096 | 12.477 | 0.722 | 5.63% | 12.371 | 14.367 |
| `native_class/reload_baseline/class/reentry/real_time` | 64 | 115.173 | 1.689 | 1.47% | 113.004 | 117.732 |
| `native_class/reload_baseline/class/repeated/real_time` | 4,096 | 56.005 | 3.400 | 5.87% | 55.699 | 63.278 |
| `native_class/reload_baseline/class/setter/real_time` | 4,096 | 9.190 | 0.125 | 1.35% | 9.102 | 9.471 |
| `native_class/reload_baseline/class/shared/real_time` | 64 | 130.033 | 2.039 | 1.58% | 126.135 | 131.679 |
| `native_class/reload_baseline/class/static/real_time` | 4,096 | 4.276 | 0.049 | 1.14% | 4.209 | 4.350 |
| `native_class/reload_baseline/class/unique/real_time` | 64 | 129.369 | 12.125 | 8.94% | 121.832 | 154.264 |
| `native_class/reload_baseline/class/void/real_time` | 4,096 | 9.299 | 0.309 | 3.29% | 9.156 | 10.070 |
| `native_class/reload_baseline/free/alternating/real_time` | 4,096 | 9.970 | 0.794 | 7.73% | 9.487 | 11.492 |
| `native_class/reload_baseline/free/boolean/real_time` | 4,096 | 9.469 | 1.704 | 16.45% | 9.316 | 13.690 |
| `native_class/reload_baseline/free/bound/real_time` | 4,096 | 33.716 | 1.898 | 5.50% | 33.450 | 38.696 |
| `native_class/reload_baseline/free/construct/real_time` | 64 | 138.143 | 2.179 | 1.59% | 133.085 | 139.725 |
| `native_class/reload_baseline/free/dynamic/real_time` | 4,096 | 242.014 | 22.319 | 8.72% | 236.218 | 288.224 |
| `native_class/reload_baseline/free/empty/real_time` | 4,096 | 19.822 | 0.333 | 1.69% | 19.313 | 20.203 |
| `native_class/reload_baseline/free/failure/real_time` | 16 | 398.286 | 34.163 | 8.22% | 395.465 | 483.994 |
| `native_class/reload_baseline/free/floating/real_time` | 4,096 | 9.039 | 0.166 | 1.82% | 8.979 | 9.404 |
| `native_class/reload_baseline/free/gc/real_time` | 16 | 1071.328 | 41.884 | 3.86% | 1039.624 | 1158.209 |
| `native_class/reload_baseline/free/getter/real_time` | 4,096 | 9.299 † | 2.444 | 23.76% | 9.005 | 15.757 |
| `native_class/reload_baseline/free/integer/real_time` | 4,096 | 9.024 | 0.046 | 0.50% | 8.987 | 9.128 |
| `native_class/reload_baseline/free/nullable/real_time` | 4,096 | 12.439 | 0.921 | 7.12% | 12.331 | 14.505 |
| `native_class/reload_baseline/free/reentry/real_time` | 64 | 113.533 | 0.979 | 0.86% | 112.578 | 115.652 |
| `native_class/reload_baseline/free/repeated/real_time` | 4,096 | 55.852 | 0.327 | 0.59% | 55.405 | 56.297 |
| `native_class/reload_baseline/free/setter/real_time` | 4,096 | 9.042 | 0.049 | 0.55% | 9.009 | 9.136 |
| `native_class/reload_baseline/free/shared/real_time` | 64 | 131.350 | 8.567 | 6.44% | 126.051 | 148.954 |
| `native_class/reload_baseline/free/static/real_time` | 4,096 | 4.262 | 0.044 | 1.02% | 4.211 | 4.352 |
| `native_class/reload_baseline/free/unique/real_time` | 64 | 130.137 | 24.784 | 17.75% | 128.765 | 195.721 |
| `native_class/reload_baseline/free/void/real_time` | 4,096 | 9.165 | 1.191 | 12.33% | 9.031 | 12.322 |
| `native_class/reload_optimized/class/alternating/real_time` | 4,096 | 9.754 | 0.135 | 1.38% | 9.657 | 10.034 |
| `native_class/reload_optimized/class/boolean/real_time` | 4,096 | 9.401 | 0.262 | 2.74% | 9.384 | 10.060 |
| `native_class/reload_optimized/class/bound/real_time` | 4,096 | 8.818 | 0.063 | 0.71% | 8.794 | 8.971 |
| `native_class/reload_optimized/class/construct/real_time` | 64 | 138.779 | 3.667 | 2.62% | 135.095 | 145.336 |
| `native_class/reload_optimized/class/dynamic/real_time` | 4,096 | 374.645 | 59.686 | 14.87% | 372.153 | 533.472 |
| `native_class/reload_optimized/class/empty/real_time` | 4,096 | 9.306 | 0.422 | 4.46% | 9.074 | 10.265 |
| `native_class/reload_optimized/class/failure/real_time` | 16 | 402.761 | 23.733 | 5.71% | 396.211 | 454.146 |
| `native_class/reload_optimized/class/floating/real_time` | 4,096 | 9.102 | 0.345 | 3.74% | 8.997 | 9.994 |
| `native_class/reload_optimized/class/gc/real_time` | 16 | 1052.452 | 38.060 | 3.55% | 1043.745 | 1141.718 |
| `native_class/reload_optimized/class/getter/real_time` | 4,096 | 9.038 | 0.149 | 1.64% | 8.973 | 9.411 |
| `native_class/reload_optimized/class/integer/real_time` | 4,096 | 9.044 | 0.154 | 1.69% | 9.020 | 9.447 |
| `native_class/reload_optimized/class/nullable/real_time` | 4,096 | 12.403 | 1.273 | 9.83% | 12.342 | 15.806 |
| `native_class/reload_optimized/class/reentry/real_time` | 64 | 113.231 | 1.592 | 1.40% | 112.234 | 116.641 |
| `native_class/reload_optimized/class/repeated/real_time` | 4,096 | 56.559 | 4.688 | 8.01% | 55.951 | 68.926 |
| `native_class/reload_optimized/class/setter/real_time` | 4,096 | 9.235 | 1.219 | 12.50% | 9.118 | 12.476 |
| `native_class/reload_optimized/class/shared/real_time` | 64 | 130.932 | 2.240 | 1.72% | 126.986 | 134.177 |
| `native_class/reload_optimized/class/static/real_time` | 4,096 | 4.281 | 0.142 | 3.28% | 4.209 | 4.623 |
| `native_class/reload_optimized/class/unique/real_time` | 64 | 132.231 | 9.539 | 7.09% | 127.162 | 155.559 |
| `native_class/reload_optimized/class/void/real_time` | 4,096 | 9.263 | 0.113 | 1.22% | 9.143 | 9.426 |
| `native_class/reload_optimized/free/alternating/real_time` | 4,096 | 9.477 | 0.067 | 0.71% | 9.448 | 9.621 |
| `native_class/reload_optimized/free/boolean/real_time` | 4,096 | 9.448 | 0.879 | 8.90% | 9.293 | 11.665 |
| `native_class/reload_optimized/free/bound/real_time` | 4,096 | 19.912 | 3.132 | 14.51% | 19.817 | 27.900 |
| `native_class/reload_optimized/free/construct/real_time` | 64 | 137.198 † | 30.376 | 20.56% | 132.259 | 216.444 |
| `native_class/reload_optimized/free/dynamic/real_time` | 4,096 | 238.106 | 14.965 | 6.16% | 235.247 | 276.466 |
| `native_class/reload_optimized/free/empty/real_time` | 4,096 | 19.763 | 0.686 | 3.45% | 19.293 | 21.353 |
| `native_class/reload_optimized/free/failure/real_time` | 16 | 398.601 | 9.029 | 2.24% | 396.716 | 419.454 |
| `native_class/reload_optimized/free/floating/real_time` | 4,096 | 9.327 | 0.481 | 5.07% | 8.985 | 10.374 |
| `native_class/reload_optimized/free/gc/real_time` | 16 | 1046.144 | 25.420 | 2.41% | 1034.479 | 1106.207 |
| `native_class/reload_optimized/free/getter/real_time` | 4,096 | 9.004 | 0.092 | 1.01% | 8.965 | 9.211 |
| `native_class/reload_optimized/free/integer/real_time` | 4,096 | 9.013 | 0.195 | 2.14% | 8.971 | 9.520 |
| `native_class/reload_optimized/free/nullable/real_time` | 4,096 | 12.453 | 2.397 | 17.95% | 12.328 | 18.779 |
| `native_class/reload_optimized/free/reentry/real_time` | 64 | 115.605 | 15.272 | 12.20% | 112.384 | 147.480 |
| `native_class/reload_optimized/free/repeated/real_time` | 4,096 | 55.987 | 0.864 | 1.54% | 55.590 | 58.056 |
| `native_class/reload_optimized/free/setter/real_time` | 4,096 | 9.295 | 0.366 | 3.88% | 9.043 | 10.027 |
| `native_class/reload_optimized/free/shared/real_time` | 64 | 131.819 | 12.949 | 9.58% | 125.958 | 163.059 |
| `native_class/reload_optimized/free/static/real_time` | 4,096 | 4.277 | 0.253 | 5.75% | 4.212 | 4.917 |
| `native_class/reload_optimized/free/unique/real_time` | 64 | 129.623 | 0.869 | 0.67% | 128.146 | 130.478 |
| `native_class/reload_optimized/free/void/real_time` | 4,096 | 9.416 | 1.181 | 12.12% | 9.101 | 12.399 |

### interop：37 项

| 用例 | N | 中位数 ns | σ ns | CV | 最小 ns | 最大 ns |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `interop/aot/runtime/callback` | 1 | 136.990 | 1.617 | 1.18% | 135.506 | 140.651 |
| `interop/aot/runtime/local` | 1 | 170.024 | 5.890 | 3.40% | 168.178 | 182.258 |
| `interop/aot/runtime/object` | 1 | 99.371 | 9.700 | 9.31% | 97.503 | 120.528 |
| `interop/aot/runtime/reentry` | 1 | 342.584 | 28.708 | 8.11% | 338.495 | 418.330 |
| `interop/aot/runtime/scalar` | 1 | 114.849 | 12.409 | 10.69% | 103.998 | 135.156 |
| `interop/aot/typed/callback` | 1 | 150.145 | 1.690 | 1.13% | 147.198 | 152.045 |
| `interop/aot/typed/local` | 1 | 193.069 | 4.596 | 2.37% | 188.364 | 201.520 |
| `interop/aot/typed/object` | 1 | 103.069 | 0.445 | 0.43% | 102.475 | 103.631 |
| `interop/aot/typed/reentry` | 1 | 363.580 † | 92.634 | 22.58% | 357.408 | 604.759 |
| `interop/aot/typed/scalar` | 1 | 121.906 | 8.902 | 7.04% | 120.503 | 143.661 |
| `interop/aot/wrapper/local` | 1 | 224.140 | 33.398 | 14.00% | 217.931 | 311.654 |
| `interop/aot/wrapper/reentry` | 1 | 393.460 | 50.766 | 12.30% | 387.053 | 526.063 |
| `interop/aot/wrapper/scalar` | 1 | 147.130 | 2.444 | 1.65% | 146.397 | 152.324 |
| `interop/async/runtime/completion` | 1 | 888.332 | 186.685 | 19.40% | 872.384 | 1383.668 |
| `interop/async/typed/completion` | 1 | 944.728 | 40.200 | 4.22% | 909.336 | 1014.034 |
| `interop/jit/runtime/callback` | 1 | 132.857 † | 29.993 | 20.65% | 131.520 | 212.935 |
| `interop/jit/runtime/local` | 1 | 161.850 | 2.789 | 1.71% | 160.436 | 168.750 |
| `interop/jit/runtime/object` | 1 | 98.327 | 1.040 | 1.06% | 96.838 | 100.194 |
| `interop/jit/runtime/reentry` | 1 | 335.746 | 7.890 | 2.33% | 331.947 | 352.856 |
| `interop/jit/runtime/scalar` | 1 | 103.928 | 0.997 | 0.96% | 103.085 | 106.142 |
| `interop/jit/typed/callback` | 1 | 144.484 | 5.803 | 3.95% | 143.510 | 159.452 |
| `interop/jit/typed/local` | 1 | 188.241 | 9.836 | 5.13% | 183.711 | 212.022 |
| `interop/jit/typed/object` | 1 | 113.904 † | 35.150 | 27.12% | 101.701 | 185.302 |
| `interop/jit/typed/reentry` | 1 | 358.802 | 10.630 | 2.93% | 349.453 | 379.237 |
| `interop/jit/typed/scalar` | 1 | 120.483 † | 32.819 | 24.60% | 119.744 | 207.757 |
| `interop/type/bound` | 1 | 40.061 | 2.956 | 7.17% | 39.603 | 47.849 |
| `interop/type/cold` | 1 | 1561.948 | 31.714 | 2.02% | 1542.669 | 1635.387 |
| `interop/values/runtime/array_reference` | 1 | 14.943 | 0.934 | 6.11% | 14.871 | 17.412 |
| `interop/values/runtime/array_scalar` | 1 | 11.638 | 0.207 | 1.77% | 11.522 | 12.116 |
| `interop/values/runtime/persistent` | 1 | 48.127 | 3.620 | 7.25% | 47.203 | 56.370 |
| `interop/values/runtime/string_borrow` | 1 | 11.329 | 0.347 | 3.07% | 10.870 | 11.699 |
| `interop/values/runtime/string_copy` | 1 | 38.973 | 0.282 | 0.73% | 38.456 | 39.112 |
| `interop/values/typed/array_reference` | 1 | 24.568 | 0.761 | 3.05% | 24.298 | 26.220 |
| `interop/values/typed/array_scalar` | 1 | 21.061 | 0.729 | 3.41% | 20.535 | 22.441 |
| `interop/values/typed/persistent` | 1 | 54.600 | 7.334 | 12.85% | 53.685 | 73.686 |
| `interop/values/typed/string_borrow` | 1 | 10.948 | 0.128 | 1.17% | 10.899 | 11.208 |
| `interop/values/typed/string_copy` | 1 | 39.531 | 2.393 | 5.92% | 39.056 | 45.749 |
