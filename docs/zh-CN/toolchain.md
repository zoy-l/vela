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

`python -m tools.vl build release` 默认对 C 和 C++ 编译启用 ThinLTO，包括 GN 构建的运行库和第三方源码。Debug 构建仍生成原生目标文件。`vela_thin_lto` GN 参数记录这一编译选择。Debug 和 Release 输出目录各自使用独立的 `thinlto-cache/` 保存增量链接缓存：预编译 LLVM SDK 可能自带 ThinLTO bitcode，不受项目编译模式控制，因此 Debug 可执行程序和测试链接时也缓存 SDK 的后端编译结果。

Clang 与 LLD 使用同一固定正式版本，使 ThinLTO bitcode 的生产与消费保持一致。
开发构建和原生发行包均使用该版本的 LLVM 归档工具与 LLD 链接器。
macOS 使用 Xcode SDK 和固定版本的 `ld64.lld` 链接器。

macOS SDK 来自系统安装的 Xcode 或 Command Line Tools。`xcrun` 解析 `SDKROOT` 选择的 SDK，未设置时使用 `macosx`。可用 `SDKROOT=/path/to/MacOSX.sdk python3 -m tools.vl test` 显式选择与固定工具链兼容的已安装 SDK；编译、测试和 lint 应使用同一选择。

支持的引导编译主机平台为 Windows x64、Linux x64 和 macOS arm64。若要添加其它主机平台，
必须在同一变更中补齐对应的 LLVM 发布归档与 SHA-256、GN/Ninja CIPD 条目和 GN 工具链参数。

`vela` 目标提供源码检查、JIT 执行、原生 AOT 构建、JIT 开发会话、测试和源码工具命令。语言与运行时契约见[语言架构](architecture.md)，原生发行包和平台 SDK 的验收范围见[AOT 契约](aot.md)。

## GitHub 构建与发布

`Build` 工作流在分支推送、Pull Request 和手动运行时，分别执行 macOS arm64 与 Windows x64
的 Debug、Release 测试。每个配置使用独立 runner。Release 作业还会将经过自检的 SDK 归档和
SHA-256 校验文件上传为工作流产物。测试和产物上传成功后，清理构建输出与 ThinLTO 链接缓存，
为依赖缓存保存回收 runner 磁盘空间。

共享环境准备 action 安装 sccache 0.16.0，开启其 GitHub Actions 缓存后端，保留默认编译包装器。
固定版本的源码依赖、GN、Ninja、LLVM 与 Unicode 输入使用另一层缓存，按操作系统、架构和
依赖声明区分；Debug、Release 共用这一依赖缓存。每次运行都会执行
`gclient sync --no-history`，包括缓存命中时，以同步固定输入并运行校验 hook。缓存未命中或被
淘汰时执行正常下载和构建，构建正确性不依赖缓存是否存在。两层缓存均受 GitHub 仓库缓存配额
与淘汰规则约束。

推送与 `compiler/build_info.h` 中版本匹配、以 `v` 开头的标签，会触发 `Release` 工作流。
工作流从该标签重新构建并测试两个平台，验证归档校验和，构建与全部附件上传成功后发布 GitHub Release。
标签包含连字符时标记为预发布，也可以手动指定已有版本标签运行。只有发布作业拥有
`contents: write` 权限，构建作业只读，不需要个人访问令牌。

macOS 包探测当前选择的 Apple SDK，记录确切的 SDK 输入和 Clang 选择的最低系统版本，并携带
匹配的 LLVM 编译运行库归档。Apple SDK 文件保持外部依赖。使用安装包构建 AOT 可执行文件前，
将 `VELA_MACOS_SDK` 设为匹配 SDK 的绝对路径，可通过
`xcrun --sdk macosx --show-sdk-path` 获取；所需 SDK 版本和文件指纹记录在
`share/vela/target.json`。Windows 包在同一清单中记录所需的 MSVC 和 Windows SDK 根目录。
生成目标文件和静态库不需要最终系统 SDK 链接输入。Linux 发行打包仍要求经过审计的
`--support-spec`，不纳入本次发布工作流。
