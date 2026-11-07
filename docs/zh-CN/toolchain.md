# Vela 工具链契约

[English](../en/toolchain.md) | [**简体中文**](toolchain.md)

`DEPS` 是仓库唯一的依赖声明文件。其中的 CIPD 条目将特定主机的 GN、Ninja 和 Clang 软件包拉取到 `third_party/build_tools/<tool>/<host-platform>` 目录下。该路径被 Git 忽略，属于检出时生成的本地状态。

`tools/config.py` 从仓库根目录解析这些工具的路径，并以绝对路径形式传递给 GN 参数。生成的工具链在编译、归档、链接和格式化时均使用相同路径。这使得 Debug 和 Release 构建输出完全隔离，并避免在环境中盲目查找系统自带的工具。

`python -m tools.vl build release` 默认对 C 和 C++ 编译启用 ThinLTO，包括 GN 构建的运行库和第三方源码。Debug 构建仍生成原生目标文件。`vela_thin_lto` GN 参数记录这一选择，每个 Release 输出目录的 `thinlto-cache/` 保存增量链接缓存。

ThinLTO bitcode 需要由同一固定版本 Clang 软件包提供的链接器消费。因此，开发构建和原生发行包均使用该软件包的 LLVM 归档工具与 LLD 链接器。LLVM 开发 SDK 提供编译器后端库，其较旧的链接器不用于消费 Release 运行库归档。macOS 使用 Xcode SDK 和固定版本的 `ld64.lld` 链接器。

支持的引导编译主机平台为 Windows x64、Linux x64 和 macOS arm64。初始本地验证在 Windows x64 上运行。若要添加其它主机平台支持，必须在同一变更中补齐匹配的 CIPD 软件包条目及对应的 GN 工具链参数。

`vela` 目标提供源码检查、JIT 执行、原生 AOT 构建、JIT 开发会话、测试和源码工具命令。语言与运行时契约见[语言架构](architecture.md)，原生发行包和平台 SDK 的验收范围见[AOT 契约](aot.md)。
