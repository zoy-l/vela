# Vela

[English](README.md) | [**简体中文**](README.zh-CN.md)

Vela 是一个面向原生应用的静态类型脚本语言。仓库包含语言前端、类型化中间表示、定宽寄存器字节码虚拟机和隔离式运行时；原生 AOT 后端计划在后续阶段实现。

Vela 目前处于早期开发阶段，语言和运行时接口仍在持续演进。

Vela 的语言和运行时设计参考了 Luau、JavaScript、Dart 等现代脚本语言的实现思路。

## 使用

主要的源代码命令：

- `vela check <source.vela>`：解析并静态检查源文件，但不执行。
- `vela run <source.vela>`：将源代码经过 Typed IR 和寄存器字节码编译后，在虚拟机中执行无参数的 `main` 函数。

`compile`/`exec` 管理[字节码镜像和缓存](docs/zh-CN/compilation_cache.md)；`fmt`、`doc` 和 `lsp` 提供[源码工具](docs/zh-CN/source_tooling.md)，[VS Code 扩展](editors/vscode/README.md)提供编辑器集成。
`debug FILE` 和 `repl` 提供[调试器](docs/zh-CN/debugger.md)与[交互会话](docs/zh-CN/repl.md)。

例如：

```powershell
.\out\debug\vela.exe check examples\function_call.vela
.\out\debug\vela.exe run examples\hello.vela
.\out\debug\vela.exe run examples\language_features.vela
```

当前语言使用类似 C/C++ 的代码块和分号语法，支持整数、双精度浮点数、布尔值、字符串、数组、函数、直接调用、固定字段和方法的类、枚举、控制流、控制台输出、导入和多文件编译。整数与双精度浮点数之间不允许隐式转换。可运行示例见 [`examples/`](examples/)，功能清单见 [`docs/zh-CN/roadmap.md`](docs/zh-CN/roadmap.md)。

## 安装

Vela 当前需要从源码构建。仓库自带并管理编译器和构建工具。

首次检出后，请确保机器上可用 depot-tools 的 `gclient` 启动器，然后执行依赖同步：

```powershell
gclient sync --no-history
python -m tools.vl doctor
```

该命令会根据 [`DEPS`](DEPS) 获取固定版本的主机构建工具、Abseil、GoogleTest、Google Benchmark 和 utf8proc 源码。依赖版本和许可证记录在 [`third_party/README.md`](third_party/README.md) 中。

## 构建

项目支持的开发入口是 `python -m tools.vl`：

```powershell
python -m tools.vl config
python -m tools.vl build
python -m tools.vl build release
```

在 Windows 上，也可以使用 `bin/vl.cmd`，例如：

```powershell
.\bin\vl.cmd build
```

Debug 和 Release 输出使用相互隔离的目录。Windows 构建使用仓库提供的 `clang-cl`、`lld-link` 和 `llvm-lib`，并配合 Visual Studio C++ 库及 Windows SDK。Linux 和 macOS 构建使用仓库提供的 Clang 和 LLVM 工具，并配合对应平台 SDK 和 libc++。

项目使用 C++23，并禁用 C++ 异常。构建脚本从 `third_party/build_tools` 解析 GN、Ninja、Clang、LLVM 归档工具和分析工具，不会修改 `PATH`。

## 测试

Vela 的 C++ 测试使用 GoogleTest，覆盖 environment、frontend、compiler、runtime 和 VM：

```powershell
python -m tools.vl test
python -m tools.vl test list
```

`--` 后的参数会直接传递给 GoogleTest：

```powershell
python -m tools.vl test debug compiler -- --gtest_list_tests
python -m tools.vl test release compiler -- --gtest_filter=CompilerTest.CompilesIntegerFunction
python -m tools.vl test debug runtime -- --gtest_repeat=10 --gtest_shuffle
```

`python -m tools.vl lint` 会运行仓库的 clang-tidy 检查。使用 `python -m tools.vl lint --fix` 应用可用修复后，请先检查修改结果，再进行验证。

## 基准测试

基准测试可执行文件会单独以 Release 模式构建：

```powershell
python -m tools.vl bench
python -m tools.vl bench -- --benchmark_list_tests
python -m tools.vl bench -- --benchmark_filter=^vm/ --benchmark_repetitions=5
```

每个工作负载都会分别测量编译、首次执行和稳定状态下的 VM 执行。基准测试覆盖整数循环、函数调用、字段访问、分配与 GC，以及生成式符号表工作负载。测量边界和预期结果见 [`benchmarks/README.md`](benchmarks/README.md)。

## 架构

```text
源代码
  → 词法分析和解析
  → 名称解析和静态类型检查
  → 类型化控制流 IR
  → 寄存器字节码
  → Vela VM
```

Typed IR 是字节码后端和未来原生后端共享的语义边界。VM 使用连续寄存器帧和定宽指令，以保证解码过程可预测。每个 `Isolate` 独占自己的堆；运行时使用精确、非移动式、增量标记清除 GC，并将 VM 栈帧寄存器显式注册为 GC 根范围。

模块职责如下：

- `compiler/frontend/`：词法分析器、解析器、语法树和静态类型检查。
- `compiler/ir/`：将检查后的 AST 降低为 Typed IR。
- `compiler/`：字节码编码和 `vela` 可执行文件。
- `vm/`：字节码验证、寄存器帧和解释执行。
- `runtime/`：值、字符串、对象、类布局、堆和 Isolate。
- `examples/` 与 `benchmarks/`：可运行程序和可重复的工作负载。

更详细的契约见 [`docs/zh-CN/architecture.md`](docs/zh-CN/architecture.md) 和 [`compiler/README.md`](compiler/README.md)。

## 依赖

构建使用仓库固定版本的 GN、Ninja、Clang、LLVM 工具、Abseil、GoogleTest 和 Google Benchmark。第三方源码和许可证信息列在 [`third_party/README.md`](third_party/README.md) 中。

## 许可证

Vela 使用 [MIT License](LICENSE) 发布。
