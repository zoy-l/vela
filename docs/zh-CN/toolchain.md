# Vela 工具链契约

[English](../en/toolchain.md) | [**简体中文**](toolchain.md)

`DEPS` 是仓库唯一的依赖声明文件。其中的 CIPD 条目将特定主机的 GN、Ninja 和 Clang 软件包拉取到 `third_party/build_tools/<tool>/<host-platform>` 目录下。该路径被 Git 忽略，属于检出时生成的本地状态。

`tools/config.py` 从仓库根目录解析这些工具的路径，并以绝对路径形式传递给 GN 参数。生成的工具链在编译、归档、链接和格式化时均使用相同路径。这使得 Debug 和 Release 构建输出完全隔离，并避免在环境中盲目查找系统自带的工具。

支持的引导编译主机平台为 Windows x64、Linux x64 和 macOS arm64。初始本地验证在 Windows x64 上运行。若要添加其它主机平台支持，必须在同一变更中补齐匹配的 CIPD 软件包条目及对应的 GN 工具链参数。

`vela` 目标提供源码检查、JIT 执行、原生 AOT 构建、JIT 开发会话、测试和源码工具命令。语言与运行时契约见[语言架构](architecture.md)，原生发行包和平台 SDK 的验收范围见[AOT 契约](aot.md)。
