# 原生编译缓存

[English](../en/compilation_cache.md) | [**简体中文**](compilation_cache.md)

Vela 缓存共享 JIT 后端生成的原生目标代码。每次请求仍先检查源码；缓存不保存堆快照、
Isolate 的全局状态、宿主绑定或异步任务。

`vela run FILE` 和 `vela debug FILE` 使用工作目录下的 `.cache/vela/native/run`。
`vela test` 使用项目配置根目录下的 `.cache/vela/native/test`。
开发会话通过 `--code-cache=DIR` 指定缓存位置。

宿主可设置 `jit::SourceOptions::compilation.code_cache`，再通过 `jit::Compiler` 调用
`jit::CompileFile`、`CompileModules` 或 `CompileSource`。不设置该选项即为无缓存编译。
`CompiledImage::cache` 返回 `Disabled`、`Miss`、`Hit` 或 `Invalid`。

## 身份与检查

每次请求都会读取当前源码、解析模块、检查类型与诊断严重程度、完成泛型特化、
准备 Typed IR 并生成 LLVM IR。缓存键对生成模块、编译器构建身份、LLVM 版本、
运行时 ABI 身份和原生优化设置计算指纹。模块包含目标平台、布局、导入、函数版本和源码元数据。
调试模式和 JIT 层级会影响生成模块。

命中缓存后省去 LLVM 优化与目标文件生成，仍须将目标代码链接到当前 JIT，并在实例化时
创建全新的运行时状态。命中不会绕过源码检查；警告仍会输出，将警告改为错误后，
先前成功生成的目标文件也不能绕过检查继续执行。

文件时间戳与长度不能证明命中。源码、依赖、规范模块路径、SDK 声明及编译器变更通过
受查模块或构建身份参与缓存键。接口成员关系按运行时编号规范化，避免哈希遍历顺序
让同一源码生成不同键。

## 目标文件与发布

缓存文件命名为 `<key>.vlobj`。`VELAJIT1` 容器保存键与带校验和的原生目标文件。
读取器先校验容器和目标文件格式，再返回命中。缺失或损坏的条目从已经检查的源码重新编译。
读写失败返回明确诊断，并使该次编译请求失败。

发布先写目标目录中的临时文件，再执行原子替换；并发写入者只发布完整条目。
Windows 使用带 replace/POSIX 语义的 `SetFileInformationByHandle(FileRenameInfoEx)`。
原子可见性不等于目录元数据在系统崩溃后的持久性保证。

缓存属于可信本地编译产物。校验和只检测损坏，不认证生产者，也不能使来自不可信方的
原生代码变得安全。缓存依赖具体编译器、ABI 和目标平台，不是发行格式。

## 所有权与分发

JIT 编译结果通过共享所有者保活已链接代码和不可变元数据，可以晚于 `jit::Compiler`
及源码 AST 销毁。每次 `NativeInstance::Instantiate` 都从接收方 Isolate 绑定宿主函数、
验证签名，并创建独立的全局状态和常量根。生命周期见[原生嵌入](native_embedding.md)。

无需源码即可分发执行的产物由 `vela build` 生成，包括可执行文件、原生目标文件或
静态库，以及配套头文件和 manifest。原生缓存仍需源码，不替代这些 AOT 产物。
详见[AOT 编译](aot.md)。

