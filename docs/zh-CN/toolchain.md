# Vela 工具链契约

[English](../en/toolchain.md) | [**简体中文**](toolchain.md)

`DEPS` 是仓库唯一的依赖声明文件。其中的 CIPD 条目将特定主机的 GN、Ninja 和 Clang 软件包拉取到 `third_party/build_tools/<tool>/<host-platform>` 目录下。该路径被 Git 忽略，属于检出时生成的本地状态。

`tools/config.py` 从仓库根目录解析这些工具的路径，并以绝对路径形式传递给 GN 参数。生成的工具链在编译、归档、链接和格式化时均使用相同路径。这使得 Debug 和 Release 构建输出完全隔离，并避免在环境中盲目查找系统自带的工具。

支持的引导编译主机平台为 Windows x64、Linux x64 和 macOS arm64。初始本地验证在 Windows x64 上运行。若要添加其它主机平台支持，必须在同一变更中补齐匹配的 CIPD 软件包条目及对应的 GN 工具链参数。

`vela` 目标特意设计为引导可执行文件。它用于验证编译器目标、链接器和测试目标是否正确连接，同时保持具体的语法与运行时契约处于演进中。
