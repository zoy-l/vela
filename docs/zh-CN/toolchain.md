# Vela 工具链契约

[English](../en/toolchain.md) | [**简体中文**](toolchain.md)

`DEPS` 是仓库唯一的依赖声明文件。CIPD 条目将特定主机的 GN 和 Ninja 拉取到
`third_party/build_tools/<tool>/<host-platform>`。`llvm_sdk` hook 获取对应主机的上游
LLVM 23.1.5 正式发布归档，校验固定的 SHA-256 后，将完整软件包安装到
`third_party/build_tools/llvm_toolchain/23.1.5/<host-platform>`。Clang、LLD、LLVM 归档工具、
clang-tidy、clang-format、clangd、运行时资源和 LLVM/LibTooling 开发库均来自这一软件包。
这些路径被 Git 忽略，属于检出时生成的本地状态。已有检出可用
`python -m tools.vl llvm sync` 执行同一 LLVM 获取流程。

`tools/config.py` 从仓库根目录解析这些工具的路径，并以绝对路径形式传递给 GN 参数。生成的工具链在编译、归档、链接和格式化时均使用相同路径。这使得 Debug 和 Release 构建输出完全隔离，并避免在环境中盲目查找系统自带的工具。

`python -m tools.vl build release` 默认对 C 和 C++ 编译启用 ThinLTO，包括 GN 构建的运行库和第三方源码。Debug 构建仍生成原生目标文件。`vela_thin_lto` GN 参数记录这一选择，每个 Release 输出目录的 `thinlto-cache/` 保存增量链接缓存。

Clang 与 LLD 使用同一固定正式版本，使 ThinLTO bitcode 的生产与消费保持一致。
开发构建和原生发行包均使用该版本的 LLVM 归档工具与 LLD 链接器。
macOS 使用 Xcode SDK 和固定版本的 `ld64.lld` 链接器。

macOS SDK 来自系统安装的 Xcode 或 Command Line Tools。`xcrun` 解析 `SDKROOT` 选择的 SDK，未设置时使用 `macosx`。可用 `SDKROOT=/path/to/MacOSX.sdk python3 -m tools.vl test` 显式选择与固定工具链兼容的已安装 SDK；编译、测试和 lint 应使用同一选择。

支持的引导编译主机平台为 Windows x64、Linux x64 和 macOS arm64。若要添加其它主机平台，
必须在同一变更中补齐对应的 LLVM 发布归档与 SHA-256、GN/Ninja CIPD 条目和 GN 工具链参数。

`vela` 目标提供源码检查、JIT 执行、原生 AOT 构建、JIT 开发会话、测试和源码工具命令。语言与运行时契约见[语言架构](architecture.md)，原生发行包和平台 SDK 的验收范围见[AOT 契约](aot.md)。
