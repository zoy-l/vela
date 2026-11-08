# Vela 与 C++ 互操作基准报告

测量时间：2026-11-05 14:32:56 至 2026-11-05 14:42:55，北京时间（UTC+8）。

本报告记录上述测量时间的 C++ 互操作实现，覆盖当时全部宿主类基准，以及标量绑定和类型化宿主 API 基准：**279 项，每项 7 次，共 1953 个有效重复样本**。所有预期用例、校验和与用例自身的资源检查通过。这是历史互操作微基准报告；算法吞吐、编译时间、热重载事务耗时和多线程调度不在本次测量范围。

宿主类全部 190 项脚本场景在完整 GC 后的额外记账存量均为 **0 B**，包括此前各保留 24 KiB 的 30 项对象创建场景；宿主构造、析构数量配对，共享所有者增量为 0。

本轮优化针对上一轮 AOT / C++ pointer 超过 1.50× 的 integer、alternating、nullable、getter，前后数据见下表。表中的倍数均表示“被测路径耗时 ÷ 对照路径耗时”，越低越好。宿主 C++ 原本使用 -O3，本轮也将 Vela 的共享 LLVM Release 后端从 O2 改为 O3。

整数宿主方法的 C++ 指针对照为 **1.291 ns/次**，闭合 Optimized JIT 类绑定为 **1.648 ns/次**（1.277×），可重载 Optimized 为 **9.298 ns/次**（7.203×）。可重载路径的额外成本在当前数据中明显可见。

另一方向的完整类型化入口 `C++ → Vela → C++` 为 **122.984 ns/次**，对应低层受查运行时入口为 **104.109 ns/次**，包装层差值 **18.874 ns/次**。它与批量脚本循环中的单次宿主调用具有不同的计时边界。

本轮有 **23 / 279 项 CV 超过 20%**，相关时间以 `†` 标记。这些结果只能支持当前机器上的描述性比较，微小差异不足以证明提升或退化。完整统计见附录，不能把接近 1 的比值解释为已经证明“零开销”。

## 本轮 AOT 优化

| 工作量 | 优化前 AOT ns | 前 AOT/C++ | 当前 AOT ns | 当前 C++ ns | 当前 AOT/C++ | AOT 耗时下降 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| integer | 2.198 | 1.701× | 1.618 | 1.291 | 1.253× | 26.4% |
| alternating | 2.576 | 1.974× | 1.634 | 1.288 | 1.269× | 36.6% |
| nullable | 4.563 | 2.169× | 2.426 | 2.099 | 1.156× | 46.8% |
| getter | 2.182 | 1.679× | 1.618 | 1.291 | 1.253× | 25.9% |
| boolean | 2.554 | 1.200× | 1.618 | 2.102 | 0.770× | 36.6% |

当前 10 项具有 C++ pointer 对照的 AOT 宿主类场景均不超过 1.50×，最高为 1.269×。

直接入口统一首位上下文和寄存器结果，调用点从保根包装借用原生指针；布尔参数和结果使用原生 bool ABI 及零扩展属性，消除阻断尾调用的字节转换；全局存储地址和调用深度在每次进入时复用，重复初始化检查利用本次成功访问的事实消除。可变全局值、宿主重入、GC 根、严格浮点、受查整数和逻辑帧契约均保留。本轮使用 O3；上述前后结果包含这些修改的共同影响。原生 ABI 已由 16 升至 17，旧 AOT 产物需要重新编译。

## 1. 环境与证据

| 项目 | 记录 |
| --- | ---: |
| CPU / 架构 | Apple M1 Pro / arm64，8 个逻辑 CPU |
| 操作系统 | Darwin 27.0.0 |
| 宿主编译器 | Fuchsia clang version 23.0.0git (https://llvm.googlesource.com/llvm-project 80743bd43fd5b38fedc503308e7a652e23d3ec93) |
| LLVM / Clang tooling SDK | 21.1.8 / 21.1.8 |
| 宿主构建 | `Release; -O3 -DNDEBUG -std=c++23 -fno-exceptions` |
| Vela 代码优化 | JIT / AOT 使用 LLVM O3；Baseline 与 Optimized 是 Vela 代码生成层级，均非 O0 |
| Google Benchmark | 1.9.5 |
| 源码提交 | `3f85f17f4e290de263463f5398fa5d30fd95435a`，包含未提交修改；完整状态与文件 SHA-256 见摘要 |
| 编译器构建身份 | `557ddd903e2fb580e256e14b40718770193887f3bd867ddbb9df20b26d466696/release` |
| 结果摘要 SHA-256 | `f6833d86293ff630a1b9978e85032b0b77dd2420280b18c06eeb9474bd71db20` |

机器可读证据保存在 Git 提交 `81ee407ca403bec1c150d7918f600ee8fdd4863b` 的 `benchmarks/results/rolling_summary.json`，该文件的 SHA-256 与上表一致；其中 `latest`、`previous` 分别记录本报告当轮和优化前的 279 项数据。可通过 `git show 81ee407ca403bec1c150d7918f600ee8fdd4863b:benchmarks/results/rolling_summary.json` 读取。[工作区滚动摘要](../../benchmarks/results/rolling_summary.json)持续保留最近两轮结果，其用例范围和机器配置可能已与本报告不同。

Vela 后端由 O2 改为 O3，直接 ABI 及 GC 索引策略也有变化，因此不将前后差异归因于单一修改，也不给出整套前后加速比。本报告的 C++ 比较均从同一轮结果中配对计算。原始重复样本不作为仓库历史保存。

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

4096 次批量下，整数宿主调用的中位数为 C++ 直接调用 **2.108 ns/次**、Vela JIT 调用 C++ **3.558 ns/次**，时间比为 **1.688×**，绝对差为 **1.451 ns/次**。

`empty` 保留空调用边界；`integer` 计算 `value + 1`；`double` 计算 `value + 0.5`；`string_borrow` 对固定的 `vela-native` 取长度及首字节之和。内核接收 `NativeContext&`，这些内核不使用句柄。JIT 使用默认 Baseline 层级。全部时间单位为 ns/次。

| 工作量 | 每批次数 | C++ 直接 | C++ 间接 | Vela JIT → C++ | JIT / 直接 | JIT / 间接 | JIT − 直接 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| empty | 64 | 1.612 | 1.611 | 3.984 | 2.472× | 2.473× | +2.372 |
| empty | 4096 | 2.442 | 2.474 † | 3.549 | 1.454× | 1.435× | +1.107 |
| integer | 64 | 1.407 | 1.428 | 4.015 | 2.854× | 2.812× | +2.609 |
| integer | 4096 | 2.108 | 2.170 | 3.558 | 1.688× | 1.640× | +1.451 |
| double | 64 | 3.558 | 3.560 | 4.888 | 1.374× | 1.373× | +1.330 |
| double | 4096 | 3.553 | 3.550 | 4.593 | 1.293× | 1.294× | +1.040 |
| string_borrow | 64 | 1.080 | 1.042 | 12.643 | 11.706× | 12.132× | +11.563 |
| string_borrow | 4096 | 0.962 | 0.972 † | 12.015 | 12.484× | 12.362× | +11.052 |

64 次批量更容易暴露每次进入脚本的固定成本；4096 次批量更接近热循环内的摊销成本。两种批量都保留，不能以大批量结果代替应用中的单次入口延迟。

| 工作量 | 每批次数 | JIT control | JIT 宿主调用 | 独立中位数之差 |
| --- | ---: | ---: | ---: | ---: |
| empty | 64 | 0.491 | 3.984 | +3.493 |
| empty | 4096 | 0.336 | 3.549 | +3.212 |
| integer | 64 | 0.490 | 4.015 | +3.526 |
| integer | 4096 | 0.340 | 3.558 | +3.218 |
| double | 64 | 0.503 | 4.888 | +4.385 |
| double | 4096 | 0.965 | 4.593 | +3.628 |
| string_borrow | 64 | 0.489 † | 12.643 | +12.153 |
| string_borrow | 4096 | 0.008 | 12.015 | +12.007 |

control 可能被编译器整体化简，字符串 control 直接使用已知结果。两列之差来自独立重复的中位数，包含生成代码变化，不是隔离后的单次跨语言 ABI 成本。

## 4. 宿主类：与 C++ 指针及手写适配器比较

这一组比较相同 `NativeClassHost` 方法。`cpp_pointer` 与 `cpp_adapter` 都通过被隐藏目标的函数指针调用，不能与第 3 节的直接 C++ 基线混称。表中每种 Vela 模式显示“ns/次（相对 C++ pointer 的时间比）”。这些项目每批均为 4096 次；脚本在每批开始重置接收者状态，其固定成本包含在结果中。

| 工作量 | C++ pointer | C++ adapter | AOT | JIT Baseline | JIT Optimized | Reload Baseline | Reload Optimized |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| empty | 2.077 | 2.073 | 1.888 †（0.909×） | 1.895 †（0.912×） | 1.883 †（0.907×） | 9.166（4.414×） | 9.223 †（4.442×） |
| integer | 1.291 | 1.290 † | 1.618（1.253×） | 1.658 †（1.284×） | 1.648（1.277×） | 9.132（7.074×） | 9.298（7.203×） |
| alternating | 1.288 | 1.307 | 1.634（1.269×） | 1.623（1.260×） | 1.626（1.262×） | 9.337（7.248×） | 9.337（7.248×） |
| floating | 3.550 | 3.578 | 1.615（0.455×） | 1.611（0.454×） | 1.613（0.454×） | 9.189 †（2.589×） | 8.867（2.498×） |
| boolean | 2.102 | 2.056 | 1.618（0.770×） | 1.621（0.771×） | 1.621（0.771×） | 9.193（4.374×） | 9.198 †（4.377×） |
| nullable | 2.099 | 2.115 | 2.426（1.156×） | 2.526（1.204×） | 2.475（1.179×） | 12.686（6.045×） | 12.725（6.063×） |
| void | 2.095 | 2.103 | 1.852（0.884×） | 1.834（0.875×） | 1.860（0.888×） | 9.067（4.329×） | 9.205（4.395×） |
| getter | 1.291 | 1.411 | 1.618（1.253×） | 1.624（1.257×） | 1.613（1.249×） | 9.164（7.097×） | 9.165（7.097×） |
| setter | 2.122 | 2.121 | 1.879（0.885×） | 1.852（0.873×） | 1.841 †（0.867×） | 9.167（4.320×） | 9.228（4.349×） |
| static | 1.341 † | 1.294 | 1.303（0.972×） | 1.293（0.964×） | 1.343 †（1.002×） | 4.256（3.174×） | 4.255（3.173×） |

`alternating` 交替使用两个接收者；`nullable` 交替传入空指针和有效对象；`void`、`getter`、`setter`、`static` 分别覆盖无返回值、属性读写和静态方法。其余 9 种场景没有对应的原生 C++ 基准，完整数据在下一节列出；不以不同语义的整数调用充当对照。

## 5. 全部宿主类路径：类绑定与自由函数绑定

下列 5 张表覆盖 190 项 Vela 路径，单位为 ns/次，比例为 class / free。`aot` 为离线代码；`baseline` / `optimized` 为闭合 JIT；`reload_baseline` / `reload_optimized` 为可重载发布中的稳态调用。可重载项目没有在计时循环中执行源码变更或重载事务。

`bound` 每批创建一次绑定方法或等价闭包后调用 4096 次；`dynamic` 使用动态分派；`construct`、`unique`、`shared` 每批创建 64 个对象并读取属性；`repeated` 重复返回同一个共享对象；`failure` 每批捕获 16 次宿主错误；`gc` 每批显式请求 16 次 GC；`reentry` 每批完成 64 次脚本 → C++ → 脚本往返。异常与重入的 ns/次包含整条操作链。

### aot

| 工作量 | 每批次数 | 类绑定 | 自由函数绑定 | class / free | 差值 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 4,096 | 1.888 † | 1.911 † | 0.988× | -0.023 |
| integer | 4,096 | 1.618 | 1.616 | 1.001× | +0.002 |
| alternating | 4,096 | 1.634 | 1.656 | 0.987× | -0.022 |
| floating | 4,096 | 1.615 | 1.618 | 0.998× | -0.003 |
| boolean | 4,096 | 1.618 | 1.626 | 0.995× | -0.008 |
| nullable | 4,096 | 2.426 | 2.433 | 0.997× | -0.007 |
| void | 4,096 | 1.852 | 1.852 | 1.000× | -0.000 |
| getter | 4,096 | 1.618 | 1.631 | 0.992× | -0.013 |
| setter | 4,096 | 1.879 | 1.862 | 1.009× | +0.017 |
| static | 4,096 | 1.303 | 1.300 | 1.002× | +0.002 |
| bound | 4,096 | 2.008 | 11.149 | 0.180× | -9.141 |
| dynamic | 4,096 | 290.537 | 249.436 | 1.165× | +41.101 |
| construct | 64 | 143.089 | 142.731 | 1.003× | +0.358 |
| unique | 64 | 133.209 | 132.814 | 1.003× | +0.395 |
| shared | 64 | 134.699 | 136.322 | 0.988× | -1.623 |
| repeated | 4,096 | 48.850 | 49.717 | 0.983× | -0.867 |
| failure | 16 | 419.277 | 400.698 | 1.046× | +18.578 |
| gc | 16 | 1164.270 | 1156.654 | 1.007× | +7.616 |
| reentry | 64 | 78.782 | 78.825 | 0.999× | -0.043 |

### baseline

| 工作量 | 每批次数 | 类绑定 | 自由函数绑定 | class / free | 差值 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 4,096 | 1.895 † | 1.918 | 0.988× | -0.023 |
| integer | 4,096 | 1.658 † | 1.652 | 1.003× | +0.005 |
| alternating | 4,096 | 1.623 | 1.613 | 1.006× | +0.010 |
| floating | 4,096 | 1.611 | 1.629 | 0.989× | -0.018 |
| boolean | 4,096 | 1.621 | 1.610 | 1.007× | +0.011 |
| nullable | 4,096 | 2.526 | 2.419 | 1.044× | +0.106 |
| void | 4,096 | 1.834 | 1.839 | 0.997× | -0.005 |
| getter | 4,096 | 1.624 | 1.675 | 0.969× | -0.052 |
| setter | 4,096 | 1.852 | 1.844 | 1.004× | +0.008 |
| static | 4,096 | 1.293 | 1.300 | 0.994× | -0.008 |
| bound | 4,096 | 38.194 | 36.599 | 1.044× | +1.596 |
| dynamic | 4,096 | 292.951 | 252.822 | 1.159× | +40.129 |
| construct | 64 | 140.291 | 140.516 | 0.998× | -0.224 |
| unique | 64 | 134.231 | 135.974 | 0.987× | -1.743 |
| shared | 64 | 134.181 | 134.096 | 1.001× | +0.085 |
| repeated | 4,096 | 48.959 | 50.178 | 0.976× | -1.220 |
| failure | 16 | 397.734 | 401.668 | 0.990× | -3.935 |
| gc | 16 | 1147.638 | 1157.364 | 0.992× | -9.725 |
| reentry | 64 | 79.927 | 80.943 | 0.987× | -1.017 |

### optimized

| 工作量 | 每批次数 | 类绑定 | 自由函数绑定 | class / free | 差值 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 4,096 | 1.883 † | 1.975 | 0.954× | -0.092 |
| integer | 4,096 | 1.648 | 1.634 | 1.009× | +0.014 |
| alternating | 4,096 | 1.626 | 1.666 | 0.976× | -0.040 |
| floating | 4,096 | 1.613 | 1.641 | 0.983× | -0.028 |
| boolean | 4,096 | 1.621 | 1.629 † | 0.995× | -0.007 |
| nullable | 4,096 | 2.475 | 2.437 | 1.016× | +0.038 |
| void | 4,096 | 1.860 | 1.859 † | 1.001× | +0.002 |
| getter | 4,096 | 1.613 | 1.625 | 0.993× | -0.011 |
| setter | 4,096 | 1.841 † | 1.832 | 1.005× | +0.008 |
| static | 4,096 | 1.343 † | 1.304 | 1.030× | +0.039 |
| bound | 4,096 | 1.480 | 11.009 | 0.134× | -9.529 |
| dynamic | 4,096 | 290.839 | 250.049 | 1.163× | +40.790 |
| construct | 64 | 141.634 | 141.789 | 0.999× | -0.154 |
| unique | 64 | 133.994 | 140.217 | 0.956× | -6.223 |
| shared | 64 | 134.864 | 135.171 | 0.998× | -0.306 |
| repeated | 4,096 | 50.058 | 49.528 | 1.011× | +0.529 |
| failure | 16 | 396.762 | 396.655 | 1.000× | +0.107 |
| gc | 16 | 1159.221 | 1154.429 | 1.004× | +4.793 |
| reentry | 64 | 79.410 | 79.650 | 0.997× | -0.239 |

### reload_baseline

| 工作量 | 每批次数 | 类绑定 | 自由函数绑定 | class / free | 差值 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 4,096 | 9.166 | 19.581 | 0.468× | -10.415 |
| integer | 4,096 | 9.132 | 9.098 | 1.004× | +0.034 |
| alternating | 4,096 | 9.337 | 9.605 | 0.972× | -0.269 |
| floating | 4,096 | 9.189 † | 8.751 | 1.050× | +0.438 |
| boolean | 4,096 | 9.193 | 9.161 | 1.004× | +0.032 |
| nullable | 4,096 | 12.686 | 12.943 † | 0.980× | -0.257 |
| void | 4,096 | 9.067 | 8.982 | 1.009× | +0.084 |
| getter | 4,096 | 9.164 | 8.816 | 1.039× | +0.348 |
| setter | 4,096 | 9.167 | 9.022 | 1.016× | +0.145 |
| static | 4,096 | 4.256 | 4.316 | 0.986× | -0.060 |
| bound | 4,096 | 26.473 | 34.450 | 0.768× | -7.976 |
| dynamic | 4,096 | 372.613 | 237.487 | 1.569× | +135.126 |
| construct | 64 | 154.382 | 151.673 | 1.018× | +2.709 |
| unique | 64 | 143.563 | 143.320 | 1.002× | +0.243 |
| shared | 64 | 146.516 | 146.087 | 1.003× | +0.429 |
| repeated | 4,096 | 56.879 | 56.532 | 1.006× | +0.347 |
| failure | 16 | 404.908 | 411.659 | 0.984× | -6.751 |
| gc | 16 | 1055.627 | 1053.511 | 1.002× | +2.116 |
| reentry | 64 | 112.898 | 111.912 † | 1.009× | +0.986 |

### reload_optimized

| 工作量 | 每批次数 | 类绑定 | 自由函数绑定 | class / free | 差值 ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 4,096 | 9.223 † | 19.339 | 0.477× | -10.116 |
| integer | 4,096 | 9.298 | 9.008 | 1.032× | +0.291 |
| alternating | 4,096 | 9.337 | 9.388 | 0.995× | -0.051 |
| floating | 4,096 | 8.867 | 8.833 | 1.004× | +0.034 |
| boolean | 4,096 | 9.198 † | 9.102 | 1.011× | +0.096 |
| nullable | 4,096 | 12.725 | 12.517 | 1.017× | +0.208 |
| void | 4,096 | 9.205 | 9.197 | 1.001× | +0.008 |
| getter | 4,096 | 9.165 | 8.900 | 1.030× | +0.265 |
| setter | 4,096 | 9.228 | 9.066 | 1.018× | +0.162 |
| static | 4,096 | 4.255 | 4.236 | 1.004× | +0.019 |
| bound | 4,096 | 8.912 | 19.888 | 0.448× | -10.976 |
| dynamic | 4,096 | 380.978 | 240.787 | 1.582× | +140.192 |
| construct | 64 | 153.598 | 154.011 | 0.997× | -0.413 |
| unique | 64 | 148.124 † | 145.068 | 1.021× | +3.056 |
| shared | 64 | 144.853 | 142.231 | 1.018× | +2.621 |
| repeated | 4,096 | 56.298 | 57.239 | 0.984× | -0.940 |
| failure | 16 | 403.683 | 404.678 | 0.998× | -0.995 |
| gc | 16 | 1052.875 | 1052.815 | 1.000× | +0.060 |
| reentry | 64 | 112.367 | 112.083 | 1.003× | +0.284 |

## 6. 类型化 C++ API：与低层 Vela 运行时比较

这一节测量公共类型、句柄及入口包装的增量成本。所有基线都使用 Vela 的值和生命周期机制；时间单位为 ns/操作。JIT 使用默认 Baseline 层级。各组的准备阶段相同，操作期间都包含临时 `HandleScope` 的创建与销毁。

`scalar` 为 C++ → 脚本 → C++ `step`；`local` 为把已建数组传给脚本，经过宿主 echo 返回，检查身份并读取元素；`callback` 调用预先创建的脚本闭包；`object` 读取已创建对象的公开字段，不包含对象构造；`reentry` 每次创建函数值并经过脚本和宿主往返。

| 场景 | 低层 runtime | 类型化 typed | typed / runtime | 差值 ns |
| --- | ---: | ---: | ---: | ---: |
| jit/scalar | 104.109 | 122.984 | 1.181× | +18.874 |
| jit/local | 165.291 | 185.595 | 1.123× | +20.303 |
| jit/callback | 132.353 | 141.494 | 1.069× | +9.140 |
| jit/object | 96.874 | 103.354 | 1.067× | +6.480 |
| jit/reentry | 330.764 | 350.624 | 1.060× | +19.860 |
| aot/scalar | 102.329 | 120.837 | 1.181× | +18.508 |
| aot/local | 163.387 | 184.650 | 1.130× | +21.263 |
| aot/callback | 131.863 | 142.703 | 1.082× | +10.841 |
| aot/object | 96.869 | 102.810 | 1.061× | +5.942 |
| aot/reentry | 343.280 | 359.194 † | 1.046× | +15.915 |

### AOT 生成包装

生成包装使用公共生成接口完成完整调用；它与预先绑定的 `Entry` 具有不同的入口工作量。

| 场景 | 低层 runtime | 生成 wrapper | wrapper / runtime | 差值 ns |
| --- | ---: | ---: | ---: | ---: |
| scalar | 102.329 | 147.104 † | 1.438× | +44.775 |
| local | 163.387 | 219.455 | 1.343× | +56.068 |
| reentry | 343.280 | 388.594 | 1.132× | +45.315 |

### 内建值、持久根及 Future

字符串输入为 42 字节，数组读取索引 0 的预建元素。persistent 测量持久根的创建和释放；completion 包含创建完成能力、同线程完成 Future、读取结果及释放，不测跨线程投递延迟。

| 场景 | 低层 runtime | 类型化 typed | typed / runtime | 差值 ns |
| --- | ---: | ---: | ---: | ---: |
| values/string_borrow | 10.922 | 10.997 | 1.007× | +0.075 |
| values/string_copy | 38.980 | 40.042 † | 1.027× | +1.061 |
| values/array_scalar | 11.975 | 20.717 | 1.730× | +8.742 |
| values/array_reference | 14.780 | 24.467 | 1.655× | +9.687 |
| values/persistent | 49.762 | 55.611 | 1.118× | +5.849 |
| async/completion | 884.122 | 929.686 | 1.052× | +45.564 |

### 冷类型绑定与已绑定类型创建

| 场景 | ns/操作 | 实际计时范围 |
| --- | ---: | ---: |
| `type/cold` | 1692.298 | 新 Isolate 上绑定类型、创建空数组、销毁局部作用域和类型；Isolate 创建、显式 GC、关闭除外 |
| `type/bound` | 39.598 | 在已绑定类型上创建空数组并销毁局部作用域 |

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
| construct | 2.023529–2.023531 | 1 | 1 | 86,528–87,040 | 0 |
| unique | 2.023529–2.023531 | 1 | 1 | 86,528–87,040 | 0 |
| shared | 2.023529–2.023531 | 1 | 1 | 86,528–87,040 | 0 |
| repeated | 0 | 0 | 0 | 1,224–1,432 | 0 |
| failure | 4 | 0 | 0 | 65,456–65,536 | 0 |
| gc | 0 | 0 | 0 | 1,072–1,280 | 0 |
| reentry | 0 | 0 | 0 | 1,072–1,280 | 0 |

bound 每批分配 1 次，在 4096 次调用上摊销为约 0.000244 次/调用；dynamic 则实际存在每调用 2–5 次分配，部分路径另有每批一次的准备成本。`construct` / `unique` / `shared` 还涉及 C++ 对象自身的 new 或共享控制块；Vela 分配计数不等于所有宿主 malloc 次数。

`construct`、`unique`、`shared` 的全部 30 项场景在本轮每次重复中均为 0 B 额外保留，其余 160 项脚本场景也为 0 B，且宿主构造与析构数量配对。此前 24,576 B 增量来自 [HostIndex](../../runtime/heap/host_index.cc) 的非空弱索引容量：构造峰值后仍有全局包装存活，旧实现只在索引清空时释放整表，GC 没有收缩较大的索引。当前每轮 GC 后按存活条目缩表，并按实际存量重设下一次 GC 阈值；分配失败时保持存活对象身份，后续 GC 再尝试缩表。0 B 表示回到本轮准备基线，基线中的常驻包装及最小索引仍计费。

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

实现验证包括 2504 项 C++ 回归、213 项工具测试和 765 个 C++ 文件的静态检查。最后一次头文件生成修改的 3 项回归复查通过；独立静态链接 AOT 集成验收包含 58 项内嵌测试和 C 宿主执行。另有 3 项 macOS 发行包验收因未配置经过审核的 `VELA_TEST_TARGET_SUPPORT_SPEC` 而跳过。

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
| `native/cpp/double/4096/real_time` | 4,096 | 3.553 | 0.082 | 2.30% | 3.528 | 3.761 |
| `native/cpp/double/64/real_time` | 64 | 3.558 | 0.145 | 4.01% | 3.511 | 3.927 |
| `native/cpp/empty/4096/real_time` | 4,096 | 2.442 | 0.437 | 16.74% | 2.421 | 3.599 |
| `native/cpp/empty/64/real_time` | 64 | 1.612 | 0.064 | 3.90% | 1.592 | 1.741 |
| `native/cpp/integer/4096/real_time` | 4,096 | 2.108 | 0.030 | 1.40% | 2.086 | 2.160 |
| `native/cpp/integer/64/real_time` | 64 | 1.407 | 0.271 | 17.92% | 1.401 | 2.126 |
| `native/cpp/string_borrow/4096/real_time` | 4,096 | 0.962 | 0.029 | 2.98% | 0.960 | 1.028 |
| `native/cpp/string_borrow/64/real_time` | 64 | 1.080 | 0.145 | 12.76% | 1.071 | 1.461 |
| `native/cpp_indirect/double/4096/real_time` | 4,096 | 3.550 | 0.338 | 9.17% | 3.522 | 4.452 |
| `native/cpp_indirect/double/64/real_time` | 64 | 3.560 | 0.038 | 1.07% | 3.538 | 3.634 |
| `native/cpp_indirect/empty/4096/real_time` | 4,096 | 2.474 † | 0.834 | 29.67% | 2.440 | 4.692 |
| `native/cpp_indirect/empty/64/real_time` | 64 | 1.611 | 0.041 | 2.50% | 1.590 | 1.710 |
| `native/cpp_indirect/integer/4096/real_time` | 4,096 | 2.170 | 0.083 | 3.75% | 2.148 | 2.381 |
| `native/cpp_indirect/integer/64/real_time` | 64 | 1.428 | 0.089 | 6.02% | 1.400 | 1.642 |
| `native/cpp_indirect/string_borrow/4096/real_time` | 4,096 | 0.972 † | 0.459 | 39.54% | 0.959 | 2.195 |
| `native/cpp_indirect/string_borrow/64/real_time` | 64 | 1.042 | 0.060 | 5.66% | 1.033 | 1.203 |
| `native/jit/double/4096/real_time` | 4,096 | 4.593 | 0.267 | 5.72% | 4.492 | 5.251 |
| `native/jit/double/64/real_time` | 64 | 4.888 | 0.155 | 3.14% | 4.832 | 5.193 |
| `native/jit/empty/4096/real_time` | 4,096 | 3.549 | 0.072 | 2.01% | 3.540 | 3.720 |
| `native/jit/empty/64/real_time` | 64 | 3.984 | 0.070 | 1.75% | 3.892 | 4.102 |
| `native/jit/integer/4096/real_time` | 4,096 | 3.558 | 0.045 | 1.26% | 3.538 | 3.655 |
| `native/jit/integer/64/real_time` | 64 | 4.015 | 0.045 | 1.12% | 3.930 | 4.056 |
| `native/jit/string_borrow/4096/real_time` | 4,096 | 12.015 | 1.020 | 8.15% | 11.879 | 14.632 |
| `native/jit/string_borrow/64/real_time` | 64 | 12.643 | 0.083 | 0.66% | 12.488 | 12.746 |
| `native/jit_control/double/4096/real_time` | 4,096 | 0.965 | 0.004 | 0.42% | 0.964 | 0.973 |
| `native/jit_control/double/64/real_time` | 64 | 0.503 | 0.021 | 4.22% | 0.487 | 0.543 |
| `native/jit_control/empty/4096/real_time` | 4,096 | 0.336 | 0.002 | 0.71% | 0.335 | 0.341 |
| `native/jit_control/empty/64/real_time` | 64 | 0.491 | 0.009 | 1.84% | 0.487 | 0.512 |
| `native/jit_control/integer/4096/real_time` | 4,096 | 0.340 | 0.031 | 8.94% | 0.336 | 0.422 |
| `native/jit_control/integer/64/real_time` | 64 | 0.490 | 0.036 | 7.18% | 0.485 | 0.585 |
| `native/jit_control/string_borrow/4096/real_time` | 4,096 | 0.008 | 0.000 | 3.57% | 0.008 | 0.008 |
| `native/jit_control/string_borrow/64/real_time` | 64 | 0.489 † | 0.133 | 23.65% | 0.486 | 0.820 |

### native_class：210 项

| 用例 | N | 中位数 ns | σ ns | CV | 最小 ns | 最大 ns |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `native_class/aot/class/alternating/real_time` | 4,096 | 1.634 | 0.034 | 2.08% | 1.607 | 1.698 |
| `native_class/aot/class/boolean/real_time` | 4,096 | 1.618 | 0.023 | 1.43% | 1.607 | 1.670 |
| `native_class/aot/class/bound/real_time` | 4,096 | 2.008 | 0.403 | 18.64% | 1.994 | 3.076 |
| `native_class/aot/class/construct/real_time` | 64 | 143.089 | 4.274 | 2.96% | 140.627 | 151.691 |
| `native_class/aot/class/dynamic/real_time` | 4,096 | 290.537 | 8.691 | 2.96% | 287.462 | 312.349 |
| `native_class/aot/class/empty/real_time` | 4,096 | 1.888 † | 0.595 | 28.20% | 1.848 | 3.454 |
| `native_class/aot/class/failure/real_time` | 16 | 419.277 | 79.133 | 17.75% | 395.041 | 618.459 |
| `native_class/aot/class/floating/real_time` | 4,096 | 1.615 | 0.014 | 0.85% | 1.603 | 1.644 |
| `native_class/aot/class/gc/real_time` | 16 | 1164.270 | 43.086 | 3.69% | 1111.489 | 1239.897 |
| `native_class/aot/class/getter/real_time` | 4,096 | 1.618 | 0.044 | 2.71% | 1.606 | 1.732 |
| `native_class/aot/class/integer/real_time` | 4,096 | 1.618 | 0.093 | 5.53% | 1.605 | 1.845 |
| `native_class/aot/class/nullable/real_time` | 4,096 | 2.426 | 0.295 | 11.56% | 2.415 | 3.215 |
| `native_class/aot/class/reentry/real_time` | 64 | 78.782 | 1.111 | 1.40% | 78.005 | 81.335 |
| `native_class/aot/class/repeated/real_time` | 4,096 | 48.850 | 3.435 | 6.84% | 48.557 | 57.968 |
| `native_class/aot/class/setter/real_time` | 4,096 | 1.879 | 0.119 | 6.21% | 1.845 | 2.181 |
| `native_class/aot/class/shared/real_time` | 64 | 134.699 | 4.139 | 3.03% | 132.611 | 143.205 |
| `native_class/aot/class/static/real_time` | 4,096 | 1.303 | 0.046 | 3.47% | 1.287 | 1.411 |
| `native_class/aot/class/unique/real_time` | 64 | 133.209 | 2.032 | 1.52% | 131.353 | 137.519 |
| `native_class/aot/class/void/real_time` | 4,096 | 1.852 | 0.016 | 0.86% | 1.840 | 1.889 |
| `native_class/aot/free/alternating/real_time` | 4,096 | 1.656 | 0.193 | 11.05% | 1.605 | 2.083 |
| `native_class/aot/free/boolean/real_time` | 4,096 | 1.626 | 0.015 | 0.94% | 1.603 | 1.653 |
| `native_class/aot/free/bound/real_time` | 4,096 | 11.149 | 0.085 | 0.76% | 11.002 | 11.206 |
| `native_class/aot/free/construct/real_time` | 64 | 142.731 | 4.243 | 2.94% | 139.358 | 151.192 |
| `native_class/aot/free/dynamic/real_time` | 4,096 | 249.436 | 6.252 | 2.47% | 247.637 | 265.043 |
| `native_class/aot/free/empty/real_time` | 4,096 | 1.911 † | 0.491 | 23.28% | 1.904 | 3.221 |
| `native_class/aot/free/failure/real_time` | 16 | 400.698 | 10.575 | 2.62% | 393.733 | 422.742 |
| `native_class/aot/free/floating/real_time` | 4,096 | 1.618 | 0.008 | 0.51% | 1.610 | 1.631 |
| `native_class/aot/free/gc/real_time` | 16 | 1156.654 | 13.649 | 1.18% | 1143.512 | 1185.692 |
| `native_class/aot/free/getter/real_time` | 4,096 | 1.631 | 0.035 | 2.15% | 1.606 | 1.706 |
| `native_class/aot/free/integer/real_time` | 4,096 | 1.616 | 0.018 | 1.08% | 1.605 | 1.652 |
| `native_class/aot/free/nullable/real_time` | 4,096 | 2.433 | 0.057 | 2.34% | 2.399 | 2.562 |
| `native_class/aot/free/reentry/real_time` | 64 | 78.825 | 2.371 | 2.96% | 78.245 | 83.976 |
| `native_class/aot/free/repeated/real_time` | 4,096 | 49.717 | 3.477 | 6.84% | 48.513 | 58.535 |
| `native_class/aot/free/setter/real_time` | 4,096 | 1.862 | 0.024 | 1.28% | 1.835 | 1.906 |
| `native_class/aot/free/shared/real_time` | 64 | 136.322 | 7.661 | 5.51% | 132.979 | 155.490 |
| `native_class/aot/free/static/real_time` | 4,096 | 1.300 | 0.058 | 4.37% | 1.282 | 1.448 |
| `native_class/aot/free/unique/real_time` | 64 | 132.814 | 7.983 | 5.86% | 130.860 | 153.648 |
| `native_class/aot/free/void/real_time` | 4,096 | 1.852 | 0.016 | 0.85% | 1.841 | 1.888 |
| `native_class/baseline/class/alternating/real_time` | 4,096 | 1.623 | 0.026 | 1.57% | 1.607 | 1.677 |
| `native_class/baseline/class/boolean/real_time` | 4,096 | 1.621 | 0.031 | 1.88% | 1.603 | 1.693 |
| `native_class/baseline/class/bound/real_time` | 4,096 | 38.194 | 1.125 | 2.90% | 37.977 | 40.947 |
| `native_class/baseline/class/construct/real_time` | 64 | 140.291 | 2.073 | 1.47% | 139.030 | 145.280 |
| `native_class/baseline/class/dynamic/real_time` | 4,096 | 292.951 | 29.496 | 9.67% | 286.085 | 370.330 |
| `native_class/baseline/class/empty/real_time` | 4,096 | 1.895 † | 0.579 | 27.61% | 1.841 | 3.410 |
| `native_class/baseline/class/failure/real_time` | 16 | 397.734 | 22.165 | 5.43% | 395.049 | 456.410 |
| `native_class/baseline/class/floating/real_time` | 4,096 | 1.611 | 0.072 | 4.36% | 1.603 | 1.771 |
| `native_class/baseline/class/gc/real_time` | 16 | 1147.638 | 113.669 | 9.47% | 1113.549 | 1419.465 |
| `native_class/baseline/class/getter/real_time` | 4,096 | 1.624 | 0.030 | 1.83% | 1.610 | 1.694 |
| `native_class/baseline/class/integer/real_time` | 4,096 | 1.658 † | 0.417 | 23.05% | 1.605 | 2.748 |
| `native_class/baseline/class/nullable/real_time` | 4,096 | 2.526 | 0.085 | 3.38% | 2.416 | 2.663 |
| `native_class/baseline/class/reentry/real_time` | 64 | 79.927 | 5.041 | 6.15% | 78.610 | 92.924 |
| `native_class/baseline/class/repeated/real_time` | 4,096 | 48.959 | 0.657 | 1.33% | 48.688 | 50.660 |
| `native_class/baseline/class/setter/real_time` | 4,096 | 1.852 | 0.123 | 6.50% | 1.809 | 2.156 |
| `native_class/baseline/class/shared/real_time` | 64 | 134.181 | 13.537 | 9.76% | 131.006 | 168.887 |
| `native_class/baseline/class/static/real_time` | 4,096 | 1.293 | 0.059 | 4.45% | 1.284 | 1.437 |
| `native_class/baseline/class/unique/real_time` | 64 | 134.231 | 3.004 | 2.23% | 132.077 | 140.919 |
| `native_class/baseline/class/void/real_time` | 4,096 | 1.834 | 0.029 | 1.55% | 1.819 | 1.895 |
| `native_class/baseline/free/alternating/real_time` | 4,096 | 1.613 | 0.017 | 1.07% | 1.603 | 1.651 |
| `native_class/baseline/free/boolean/real_time` | 4,096 | 1.610 | 0.024 | 1.46% | 1.605 | 1.662 |
| `native_class/baseline/free/bound/real_time` | 4,096 | 36.599 | 0.706 | 1.92% | 36.349 | 38.411 |
| `native_class/baseline/free/construct/real_time` | 64 | 140.516 | 1.748 | 1.24% | 137.760 | 142.965 |
| `native_class/baseline/free/dynamic/real_time` | 4,096 | 252.822 | 4.203 | 1.66% | 249.421 | 261.895 |
| `native_class/baseline/free/empty/real_time` | 4,096 | 1.918 | 0.027 | 1.40% | 1.910 | 1.982 |
| `native_class/baseline/free/failure/real_time` | 16 | 401.668 | 78.663 | 18.26% | 394.075 | 608.653 |
| `native_class/baseline/free/floating/real_time` | 4,096 | 1.629 | 0.016 | 1.00% | 1.608 | 1.658 |
| `native_class/baseline/free/gc/real_time` | 16 | 1157.364 | 44.947 | 3.84% | 1118.049 | 1254.929 |
| `native_class/baseline/free/getter/real_time` | 4,096 | 1.675 | 0.065 | 3.88% | 1.612 | 1.776 |
| `native_class/baseline/free/integer/real_time` | 4,096 | 1.652 | 0.037 | 2.24% | 1.605 | 1.712 |
| `native_class/baseline/free/nullable/real_time` | 4,096 | 2.419 | 0.023 | 0.97% | 2.400 | 2.466 |
| `native_class/baseline/free/reentry/real_time` | 64 | 80.943 | 2.035 | 2.52% | 78.295 | 83.386 |
| `native_class/baseline/free/repeated/real_time` | 4,096 | 50.178 | 1.057 | 2.11% | 49.029 | 51.933 |
| `native_class/baseline/free/setter/real_time` | 4,096 | 1.844 | 0.219 | 11.41% | 1.799 | 2.412 |
| `native_class/baseline/free/shared/real_time` | 64 | 134.096 | 2.846 | 2.11% | 132.350 | 139.641 |
| `native_class/baseline/free/static/real_time` | 4,096 | 1.300 | 0.030 | 2.28% | 1.281 | 1.368 |
| `native_class/baseline/free/unique/real_time` | 64 | 135.974 | 5.599 | 4.08% | 131.845 | 146.514 |
| `native_class/baseline/free/void/real_time` | 4,096 | 1.839 | 0.031 | 1.69% | 1.817 | 1.897 |
| `native_class/cpp_adapter/alternating/real_time` | 4,096 | 1.307 | 0.259 | 18.43% | 1.291 | 1.993 |
| `native_class/cpp_adapter/boolean/real_time` | 4,096 | 2.056 | 0.034 | 1.66% | 2.021 | 2.104 |
| `native_class/cpp_adapter/empty/real_time` | 4,096 | 2.073 | 0.361 | 16.19% | 2.059 | 3.042 |
| `native_class/cpp_adapter/floating/real_time` | 4,096 | 3.578 | 0.079 | 2.20% | 3.514 | 3.746 |
| `native_class/cpp_adapter/getter/real_time` | 4,096 | 1.411 | 0.240 | 16.09% | 1.288 | 1.971 |
| `native_class/cpp_adapter/integer/real_time` | 4,096 | 1.290 † | 0.297 | 20.72% | 1.284 | 2.096 |
| `native_class/cpp_adapter/nullable/real_time` | 4,096 | 2.115 | 0.039 | 1.83% | 2.101 | 2.200 |
| `native_class/cpp_adapter/setter/real_time` | 4,096 | 2.121 | 0.037 | 1.76% | 2.097 | 2.209 |
| `native_class/cpp_adapter/static/real_time` | 4,096 | 1.294 | 0.016 | 1.24% | 1.281 | 1.323 |
| `native_class/cpp_adapter/void/real_time` | 4,096 | 2.103 | 0.032 | 1.51% | 2.091 | 2.170 |
| `native_class/cpp_pointer/alternating/real_time` | 4,096 | 1.288 | 0.069 | 5.27% | 1.281 | 1.470 |
| `native_class/cpp_pointer/boolean/real_time` | 4,096 | 2.102 | 0.070 | 3.26% | 2.076 | 2.258 |
| `native_class/cpp_pointer/empty/real_time` | 4,096 | 2.077 | 0.043 | 2.06% | 2.070 | 2.189 |
| `native_class/cpp_pointer/floating/real_time` | 4,096 | 3.550 | 0.022 | 0.63% | 3.507 | 3.573 |
| `native_class/cpp_pointer/getter/real_time` | 4,096 | 1.291 | 0.062 | 4.70% | 1.285 | 1.452 |
| `native_class/cpp_pointer/integer/real_time` | 4,096 | 1.291 | 0.115 | 8.57% | 1.281 | 1.594 |
| `native_class/cpp_pointer/nullable/real_time` | 4,096 | 2.099 | 0.030 | 1.43% | 2.079 | 2.168 |
| `native_class/cpp_pointer/setter/real_time` | 4,096 | 2.122 | 0.040 | 1.89% | 2.089 | 2.197 |
| `native_class/cpp_pointer/static/real_time` | 4,096 | 1.341 † | 0.370 | 25.24% | 1.282 | 2.297 |
| `native_class/cpp_pointer/void/real_time` | 4,096 | 2.095 | 0.038 | 1.79% | 2.085 | 2.178 |
| `native_class/optimized/class/alternating/real_time` | 4,096 | 1.626 | 0.088 | 5.30% | 1.603 | 1.844 |
| `native_class/optimized/class/boolean/real_time` | 4,096 | 1.621 | 0.029 | 1.76% | 1.599 | 1.674 |
| `native_class/optimized/class/bound/real_time` | 4,096 | 1.480 | 0.067 | 4.42% | 1.455 | 1.632 |
| `native_class/optimized/class/construct/real_time` | 64 | 141.634 | 5.393 | 3.73% | 139.618 | 153.823 |
| `native_class/optimized/class/dynamic/real_time` | 4,096 | 290.839 | 11.497 | 3.90% | 287.081 | 319.866 |
| `native_class/optimized/class/empty/real_time` | 4,096 | 1.883 † | 0.493 | 23.85% | 1.848 | 3.184 |
| `native_class/optimized/class/failure/real_time` | 16 | 396.762 | 9.248 | 2.30% | 394.911 | 420.975 |
| `native_class/optimized/class/floating/real_time` | 4,096 | 1.613 | 0.018 | 1.11% | 1.605 | 1.656 |
| `native_class/optimized/class/gc/real_time` | 16 | 1159.221 | 27.056 | 2.33% | 1119.609 | 1209.648 |
| `native_class/optimized/class/getter/real_time` | 4,096 | 1.613 | 0.014 | 0.87% | 1.601 | 1.643 |
| `native_class/optimized/class/integer/real_time` | 4,096 | 1.648 | 0.108 | 6.34% | 1.605 | 1.892 |
| `native_class/optimized/class/nullable/real_time` | 4,096 | 2.475 | 0.058 | 2.34% | 2.413 | 2.556 |
| `native_class/optimized/class/reentry/real_time` | 64 | 79.410 | 2.074 | 2.59% | 78.207 | 83.810 |
| `native_class/optimized/class/repeated/real_time` | 4,096 | 50.058 | 5.609 | 10.66% | 48.772 | 63.232 |
| `native_class/optimized/class/setter/real_time` | 4,096 | 1.841 † | 0.578 | 28.22% | 1.797 | 3.354 |
| `native_class/optimized/class/shared/real_time` | 64 | 134.864 | 12.678 | 9.04% | 131.967 | 167.410 |
| `native_class/optimized/class/static/real_time` | 4,096 | 1.343 † | 0.361 | 24.69% | 1.282 | 2.273 |
| `native_class/optimized/class/unique/real_time` | 64 | 133.994 | 12.477 | 8.85% | 131.278 | 165.251 |
| `native_class/optimized/class/void/real_time` | 4,096 | 1.860 | 0.093 | 4.93% | 1.817 | 2.088 |
| `native_class/optimized/free/alternating/real_time` | 4,096 | 1.666 | 0.172 | 9.86% | 1.607 | 1.992 |
| `native_class/optimized/free/boolean/real_time` | 4,096 | 1.629 † | 0.360 | 20.01% | 1.613 | 2.583 |
| `native_class/optimized/free/bound/real_time` | 4,096 | 11.009 | 0.358 | 3.21% | 10.729 | 11.663 |
| `native_class/optimized/free/construct/real_time` | 64 | 141.789 | 4.204 | 2.93% | 139.229 | 150.725 |
| `native_class/optimized/free/dynamic/real_time` | 4,096 | 250.049 | 51.341 | 19.06% | 247.748 | 385.801 |
| `native_class/optimized/free/empty/real_time` | 4,096 | 1.975 | 0.082 | 4.10% | 1.915 | 2.145 |
| `native_class/optimized/free/failure/real_time` | 16 | 396.655 | 17.158 | 4.25% | 395.838 | 442.504 |
| `native_class/optimized/free/floating/real_time` | 4,096 | 1.641 | 0.296 | 16.76% | 1.604 | 2.424 |
| `native_class/optimized/free/gc/real_time` | 16 | 1154.429 | 81.843 | 6.81% | 1136.311 | 1338.404 |
| `native_class/optimized/free/getter/real_time` | 4,096 | 1.625 | 0.050 | 3.06% | 1.607 | 1.752 |
| `native_class/optimized/free/integer/real_time` | 4,096 | 1.634 | 0.207 | 11.95% | 1.601 | 2.173 |
| `native_class/optimized/free/nullable/real_time` | 4,096 | 2.437 | 0.030 | 1.23% | 2.405 | 2.486 |
| `native_class/optimized/free/reentry/real_time` | 64 | 79.650 | 1.172 | 1.47% | 78.371 | 81.426 |
| `native_class/optimized/free/repeated/real_time` | 4,096 | 49.528 | 1.643 | 3.28% | 48.648 | 52.637 |
| `native_class/optimized/free/setter/real_time` | 4,096 | 1.832 | 0.048 | 2.61% | 1.803 | 1.950 |
| `native_class/optimized/free/shared/real_time` | 64 | 135.171 | 3.955 | 2.90% | 132.598 | 143.855 |
| `native_class/optimized/free/static/real_time` | 4,096 | 1.304 | 0.033 | 2.53% | 1.283 | 1.381 |
| `native_class/optimized/free/unique/real_time` | 64 | 140.217 | 17.405 | 11.71% | 132.300 | 179.912 |
| `native_class/optimized/free/void/real_time` | 4,096 | 1.859 † | 0.522 | 25.48% | 1.821 | 3.232 |
| `native_class/reload_baseline/class/alternating/real_time` | 4,096 | 9.337 | 0.289 | 3.04% | 9.311 | 10.070 |
| `native_class/reload_baseline/class/boolean/real_time` | 4,096 | 9.193 | 0.124 | 1.36% | 9.015 | 9.338 |
| `native_class/reload_baseline/class/bound/real_time` | 4,096 | 26.473 | 0.382 | 1.44% | 26.330 | 27.411 |
| `native_class/reload_baseline/class/construct/real_time` | 64 | 154.382 | 4.332 | 2.78% | 149.834 | 162.057 |
| `native_class/reload_baseline/class/dynamic/real_time` | 4,096 | 372.613 | 16.320 | 4.29% | 370.721 | 416.320 |
| `native_class/reload_baseline/class/empty/real_time` | 4,096 | 9.166 | 0.484 | 5.17% | 8.990 | 10.319 |
| `native_class/reload_baseline/class/failure/real_time` | 16 | 404.908 | 9.072 | 2.22% | 400.354 | 427.430 |
| `native_class/reload_baseline/class/floating/real_time` | 4,096 | 9.189 † | 2.965 | 28.38% | 8.818 | 16.985 |
| `native_class/reload_baseline/class/gc/real_time` | 16 | 1055.627 | 33.104 | 3.10% | 1042.406 | 1140.507 |
| `native_class/reload_baseline/class/getter/real_time` | 4,096 | 9.164 | 0.311 | 3.35% | 9.055 | 9.936 |
| `native_class/reload_baseline/class/integer/real_time` | 4,096 | 9.132 | 0.257 | 2.78% | 8.995 | 9.741 |
| `native_class/reload_baseline/class/nullable/real_time` | 4,096 | 12.686 | 0.235 | 1.85% | 12.526 | 13.159 |
| `native_class/reload_baseline/class/reentry/real_time` | 64 | 112.898 | 6.276 | 5.43% | 111.818 | 129.028 |
| `native_class/reload_baseline/class/repeated/real_time` | 4,096 | 56.879 | 2.470 | 4.27% | 55.937 | 63.137 |
| `native_class/reload_baseline/class/setter/real_time` | 4,096 | 9.167 | 0.080 | 0.87% | 9.012 | 9.236 |
| `native_class/reload_baseline/class/shared/real_time` | 64 | 146.516 | 5.566 | 3.76% | 142.907 | 159.709 |
| `native_class/reload_baseline/class/static/real_time` | 4,096 | 4.256 | 0.020 | 0.46% | 4.223 | 4.273 |
| `native_class/reload_baseline/class/unique/real_time` | 64 | 143.563 | 4.086 | 2.82% | 141.898 | 153.912 |
| `native_class/reload_baseline/class/void/real_time` | 4,096 | 9.067 | 0.720 | 7.66% | 8.988 | 10.993 |
| `native_class/reload_baseline/free/alternating/real_time` | 4,096 | 9.605 | 0.237 | 2.45% | 9.340 | 10.108 |
| `native_class/reload_baseline/free/boolean/real_time` | 4,096 | 9.161 | 0.180 | 1.96% | 8.995 | 9.479 |
| `native_class/reload_baseline/free/bound/real_time` | 4,096 | 34.450 | 0.730 | 2.10% | 34.217 | 35.892 |
| `native_class/reload_baseline/free/construct/real_time` | 64 | 151.673 | 4.384 | 2.86% | 149.036 | 160.800 |
| `native_class/reload_baseline/free/dynamic/real_time` | 4,096 | 237.487 | 13.018 | 5.36% | 235.570 | 271.435 |
| `native_class/reload_baseline/free/empty/real_time` | 4,096 | 19.581 | 1.378 | 6.82% | 19.321 | 23.016 |
| `native_class/reload_baseline/free/failure/real_time` | 16 | 411.659 | 13.973 | 3.35% | 400.643 | 436.457 |
| `native_class/reload_baseline/free/floating/real_time` | 4,096 | 8.751 | 0.174 | 1.97% | 8.701 | 9.161 |
| `native_class/reload_baseline/free/gc/real_time` | 16 | 1053.511 | 44.344 | 4.14% | 1041.326 | 1167.939 |
| `native_class/reload_baseline/free/getter/real_time` | 4,096 | 8.816 | 0.170 | 1.91% | 8.694 | 9.198 |
| `native_class/reload_baseline/free/integer/real_time` | 4,096 | 9.098 | 0.257 | 2.80% | 9.020 | 9.734 |
| `native_class/reload_baseline/free/nullable/real_time` | 4,096 | 12.943 † | 3.155 | 22.32% | 12.189 | 21.122 |
| `native_class/reload_baseline/free/reentry/real_time` | 64 | 111.912 † | 43.758 | 34.04% | 111.266 | 227.784 |
| `native_class/reload_baseline/free/repeated/real_time` | 4,096 | 56.532 | 1.719 | 2.99% | 56.101 | 60.616 |
| `native_class/reload_baseline/free/setter/real_time` | 4,096 | 9.022 | 0.126 | 1.38% | 8.976 | 9.288 |
| `native_class/reload_baseline/free/shared/real_time` | 64 | 146.087 | 5.638 | 3.83% | 142.411 | 158.388 |
| `native_class/reload_baseline/free/static/real_time` | 4,096 | 4.316 | 0.437 | 9.70% | 4.222 | 5.435 |
| `native_class/reload_baseline/free/unique/real_time` | 64 | 143.320 | 1.242 | 0.87% | 141.838 | 145.673 |
| `native_class/reload_baseline/free/void/real_time` | 4,096 | 8.982 | 0.125 | 1.38% | 8.925 | 9.287 |
| `native_class/reload_optimized/class/alternating/real_time` | 4,096 | 9.337 | 0.077 | 0.82% | 9.297 | 9.506 |
| `native_class/reload_optimized/class/boolean/real_time` | 4,096 | 9.198 † | 2.267 | 22.41% | 9.025 | 15.227 |
| `native_class/reload_optimized/class/bound/real_time` | 4,096 | 8.912 | 0.261 | 2.87% | 8.815 | 9.459 |
| `native_class/reload_optimized/class/construct/real_time` | 64 | 153.598 | 11.520 | 7.27% | 149.332 | 182.402 |
| `native_class/reload_optimized/class/dynamic/real_time` | 4,096 | 380.978 | 20.985 | 5.41% | 369.611 | 431.537 |
| `native_class/reload_optimized/class/empty/real_time` | 4,096 | 9.223 † | 2.064 | 20.70% | 9.040 | 14.637 |
| `native_class/reload_optimized/class/failure/real_time` | 16 | 403.683 | 26.626 | 6.46% | 398.486 | 472.162 |
| `native_class/reload_optimized/class/floating/real_time` | 4,096 | 8.867 | 0.861 | 9.32% | 8.749 | 11.130 |
| `native_class/reload_optimized/class/gc/real_time` | 16 | 1052.875 | 31.914 | 2.99% | 1040.906 | 1130.999 |
| `native_class/reload_optimized/class/getter/real_time` | 4,096 | 9.165 | 1.041 | 10.94% | 8.976 | 11.868 |
| `native_class/reload_optimized/class/integer/real_time` | 4,096 | 9.298 | 0.991 | 10.20% | 8.988 | 11.583 |
| `native_class/reload_optimized/class/nullable/real_time` | 4,096 | 12.725 | 0.220 | 1.73% | 12.538 | 13.177 |
| `native_class/reload_optimized/class/reentry/real_time` | 64 | 112.367 | 6.312 | 5.47% | 111.360 | 127.624 |
| `native_class/reload_optimized/class/repeated/real_time` | 4,096 | 56.298 | 0.496 | 0.88% | 55.950 | 57.236 |
| `native_class/reload_optimized/class/setter/real_time` | 4,096 | 9.228 | 0.257 | 2.77% | 9.026 | 9.731 |
| `native_class/reload_optimized/class/shared/real_time` | 64 | 144.853 | 5.716 | 3.89% | 141.696 | 157.712 |
| `native_class/reload_optimized/class/static/real_time` | 4,096 | 4.255 | 0.543 | 12.02% | 4.215 | 5.680 |
| `native_class/reload_optimized/class/unique/real_time` | 64 | 148.124 † | 34.805 | 21.70% | 142.381 | 238.743 |
| `native_class/reload_optimized/class/void/real_time` | 4,096 | 9.205 | 0.262 | 2.81% | 9.020 | 9.727 |
| `native_class/reload_optimized/free/alternating/real_time` | 4,096 | 9.388 | 0.427 | 4.45% | 9.325 | 10.481 |
| `native_class/reload_optimized/free/boolean/real_time` | 4,096 | 9.102 | 0.375 | 4.01% | 9.039 | 9.949 |
| `native_class/reload_optimized/free/bound/real_time` | 4,096 | 19.888 | 1.833 | 8.74% | 19.645 | 24.350 |
| `native_class/reload_optimized/free/construct/real_time` | 64 | 154.011 | 12.609 | 7.87% | 150.896 | 184.507 |
| `native_class/reload_optimized/free/dynamic/real_time` | 4,096 | 240.787 | 13.479 | 5.44% | 235.985 | 272.691 |
| `native_class/reload_optimized/free/empty/real_time` | 4,096 | 19.339 | 0.128 | 0.66% | 19.301 | 19.665 |
| `native_class/reload_optimized/free/failure/real_time` | 16 | 404.678 | 13.016 | 3.17% | 400.332 | 432.623 |
| `native_class/reload_optimized/free/floating/real_time` | 4,096 | 8.833 | 0.257 | 2.88% | 8.729 | 9.478 |
| `native_class/reload_optimized/free/gc/real_time` | 16 | 1052.815 | 27.391 | 2.58% | 1025.674 | 1099.587 |
| `native_class/reload_optimized/free/getter/real_time` | 4,096 | 8.900 | 0.417 | 4.61% | 8.708 | 9.969 |
| `native_class/reload_optimized/free/integer/real_time` | 4,096 | 9.008 | 0.046 | 0.50% | 8.976 | 9.100 |
| `native_class/reload_optimized/free/nullable/real_time` | 4,096 | 12.517 | 2.282 | 17.16% | 12.271 | 18.465 |
| `native_class/reload_optimized/free/reentry/real_time` | 64 | 112.083 | 1.661 | 1.47% | 111.222 | 115.943 |
| `native_class/reload_optimized/free/repeated/real_time` | 4,096 | 57.239 | 0.582 | 1.02% | 56.336 | 57.736 |
| `native_class/reload_optimized/free/setter/real_time` | 4,096 | 9.066 | 0.090 | 0.99% | 8.958 | 9.217 |
| `native_class/reload_optimized/free/shared/real_time` | 64 | 142.231 | 3.626 | 2.51% | 141.447 | 149.501 |
| `native_class/reload_optimized/free/static/real_time` | 4,096 | 4.236 | 0.072 | 1.70% | 4.203 | 4.418 |
| `native_class/reload_optimized/free/unique/real_time` | 64 | 145.068 | 9.914 | 6.67% | 140.324 | 167.829 |
| `native_class/reload_optimized/free/void/real_time` | 4,096 | 9.197 | 0.932 | 9.75% | 8.911 | 11.508 |

### interop：37 项

| 用例 | N | 中位数 ns | σ ns | CV | 最小 ns | 最大 ns |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `interop/aot/runtime/callback` | 1 | 131.863 | 1.615 | 1.22% | 130.414 | 134.915 |
| `interop/aot/runtime/local` | 1 | 163.387 | 4.940 | 2.98% | 162.259 | 176.038 |
| `interop/aot/runtime/object` | 1 | 96.869 | 0.300 | 0.31% | 96.692 | 97.524 |
| `interop/aot/runtime/reentry` | 1 | 343.280 | 9.102 | 2.68% | 330.325 | 355.285 |
| `interop/aot/runtime/scalar` | 1 | 102.329 | 3.033 | 2.93% | 101.852 | 110.255 |
| `interop/aot/typed/callback` | 1 | 142.703 | 3.919 | 2.72% | 140.522 | 151.781 |
| `interop/aot/typed/local` | 1 | 184.650 | 3.314 | 1.78% | 183.953 | 192.899 |
| `interop/aot/typed/object` | 1 | 102.810 | 1.917 | 1.86% | 101.569 | 106.723 |
| `interop/aot/typed/reentry` | 1 | 359.194 † | 90.864 | 23.05% | 355.762 | 600.031 |
| `interop/aot/typed/scalar` | 1 | 120.837 | 2.632 | 2.17% | 119.356 | 127.123 |
| `interop/aot/wrapper/local` | 1 | 219.455 | 4.869 | 2.20% | 217.544 | 230.955 |
| `interop/aot/wrapper/reentry` | 1 | 388.594 | 7.820 | 2.00% | 382.049 | 406.212 |
| `interop/aot/wrapper/scalar` | 1 | 147.104 † | 41.222 | 24.50% | 144.693 | 252.127 |
| `interop/async/runtime/completion` | 1 | 884.122 | 96.027 | 10.37% | 872.904 | 1139.746 |
| `interop/async/typed/completion` | 1 | 929.686 | 17.542 | 1.88% | 917.073 | 959.492 |
| `interop/jit/runtime/callback` | 1 | 132.353 | 1.701 | 1.28% | 130.454 | 135.795 |
| `interop/jit/runtime/local` | 1 | 165.291 | 18.108 | 10.44% | 162.908 | 213.459 |
| `interop/jit/runtime/object` | 1 | 96.874 | 1.253 | 1.29% | 96.564 | 99.825 |
| `interop/jit/runtime/reentry` | 1 | 330.764 | 13.598 | 4.04% | 329.903 | 366.873 |
| `interop/jit/runtime/scalar` | 1 | 104.109 | 1.555 | 1.50% | 101.833 | 106.455 |
| `interop/jit/typed/callback` | 1 | 141.494 | 1.018 | 0.72% | 140.332 | 143.110 |
| `interop/jit/typed/local` | 1 | 185.595 | 0.937 | 0.50% | 184.702 | 187.297 |
| `interop/jit/typed/object` | 1 | 103.354 | 3.180 | 3.06% | 101.530 | 110.897 |
| `interop/jit/typed/reentry` | 1 | 350.624 | 4.884 | 1.39% | 347.533 | 361.937 |
| `interop/jit/typed/scalar` | 1 | 122.984 | 5.709 | 4.57% | 119.775 | 136.125 |
| `interop/type/bound` | 1 | 39.598 | 0.873 | 2.18% | 39.214 | 41.331 |
| `interop/type/cold` | 1 | 1692.298 | 235.309 | 13.61% | 1523.131 | 2119.584 |
| `interop/values/runtime/array_reference` | 1 | 14.780 | 0.135 | 0.92% | 14.667 | 15.049 |
| `interop/values/runtime/array_scalar` | 1 | 11.975 | 0.107 | 0.90% | 11.782 | 12.033 |
| `interop/values/runtime/persistent` | 1 | 49.762 | 2.089 | 4.12% | 49.040 | 55.002 |
| `interop/values/runtime/string_borrow` | 1 | 10.922 | 0.240 | 2.18% | 10.882 | 11.546 |
| `interop/values/runtime/string_copy` | 1 | 38.980 | 0.805 | 2.06% | 38.329 | 40.186 |
| `interop/values/typed/array_reference` | 1 | 24.467 | 0.701 | 2.83% | 24.270 | 26.291 |
| `interop/values/typed/array_scalar` | 1 | 20.717 | 0.306 | 1.47% | 20.446 | 21.232 |
| `interop/values/typed/persistent` | 1 | 55.611 | 0.383 | 0.69% | 55.255 | 56.213 |
| `interop/values/typed/string_borrow` | 1 | 10.997 | 0.846 | 7.39% | 10.859 | 13.149 |
| `interop/values/typed/string_copy` | 1 | 40.042 † | 11.636 | 25.49% | 39.368 | 71.233 |
