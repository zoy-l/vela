# 字符串大小写转换

[English](../en/string_case.md) | [**简体中文**](string_case.md)

Vela 为不可变 `string` 提供 Dart 风格的实例方法 `toLowerCase()` 和 `toUpperCase()`。二者采用与 locale 无关的 Unicode 默认大小写转换，覆盖多码点映射及与语言无关的上下文规则。返回字符串，不修改接收者。

```vela
func main(): void {
  print('Vela'.toLowerCase());   // vela
  print('Vela'.toUpperCase());   // VELA
  print('straße'.toUpperCase()); // STRASSE
  print('ΟΣ'.toLowerCase());     // ος
  print('İ'.toLowerCase());      // i 后跟 U+0307
}
```

第一版不加入 locale 特定转换、case folding、标题大小写、规范化、builder 原地转换或忽略大小写比较 API。方法不依赖 dynamic 或 JSON。[RegExp API](regexp.md)的忽略大小写比较具有独立契约。

## API 与可观察行为

| API | 契约 |
| --- | --- |
| `text.toLowerCase(): string` | Unicode 默认小写转换 |
| `text.toUpperCase(): string` | Unicode 默认大写转换 |

接收者必须为非空 string；可空接收者使用已有空安全访问或类型收窄。方法均不接受参数。错误静态接收者或实参由前端诊断，受检查的 builtin 调用也验证运行时操作数类型。结果是普通不可变 UTF-8 字符串，可用于字段、数组、捕获和原生字符串调用。

采用固定 Unicode 18.0.0 的默认转换：无条件完整映射，加上与语言无关的上下文映射，包括 Final_Sigma。不应用土耳其语、阿塞拜疆语、立陶宛语等 locale 特定规则。默认情况下，I 转小写为 i，带点大写 I 转为 i 加组合点。

单个输入码点可以产生多个输出码点；输出字节数和码点数都可能改变，转换不保留原始索引。空输入仍为空；没有映射的无大小写字符、补充平面字符、组合标记、emoji 和内嵌 NUL 保留。上下文规则检查 Unicode Cased 和 Case_Ignorable 属性，不能使用仅针对 ASCII 的词边界。

不做规范化或去重音。完整转换可以产生分解序列，必须保留该序列。大小写转换不是可逆编码，大写后再小写不保证恢复原文。固定映射数据下，重复执行同一种转换应保持幂等。如果内容没有变化，可以复用原不可变字符串；对象分配身份不属于 API 契约。

RegExp 忽略大小写匹配不能先用这些方法转换整份输入：展开会改变偏移，也不能实现 PCRE2 的 caseless 匹配规则。

## Unicode 数据与实现

复用现有 utf8proc 依赖提供简单映射，增加生成的 Unicode 完整/上下文映射数据。大小写操作独立于正则引擎。现有 [Unicode 操作](../../runtime/unicode_operations.cc)继续通过 utf8proc 提供码点与字素簇分割。

补充数据必须与固定 utf8proc 构建采用相同 Unicode 版本：从 SpecialCasing.txt 提取无条件及与语言无关的条件条目，从 DerivedCoreProperties.txt 提取 Cased 和 Case_Ignorable 范围。存在完整/上下文条目时使用对应规则，其余码点使用 utf8proc 的公开简单映射函数。Final_Sigma 根据原始输入上下文计算。默认大小写契约排除带 locale 标签的映射，仅逐码点处理不能实现完整转换。

通过仓库依赖流程管理不可变、校验内容身份的 UCD 源输入，提供使用项目 Python 工具链的确定性离线生成器并接入 GN。生成不能依赖宿主 Unicode 数据库、会变化的 latest URL 或构建时网络。不读取 utf8proc 私有序列表，不修改上游源码。数据/schema 校验及重新生成检查由生成器承担，不在每次运行时转换中重复执行。

在既有依赖/构建记录中登记 UCD release、输入身份、生成输出、生成器和 Unicode 数据许可证，要求补充数据与 utf8proc 的 Unicode 版本一致。规划中的 PCRE2 正则引擎将独立固定 Unicode 表版本，不要求与之相同，因为它负责正则匹配而非本转换 API。Unicode 更新属于可观察变化，计入构建/缓存身份并审查测试向量。不按宿主 OS、浏览器或 locale 选择映射，不在运行时下载数据。宿主 C/C++ locale 函数或单独使用 utf8proc 简单映射均不能替代完整算法。

对已经验证的运行时字符串复用 [UTF-8 基础操作](../../base/utf8.h)，以线性扫描计算映射、上下文和受检查的输出大小，随后只分配一次最终堆字符串并直接写入合法 UTF-8。不为每个上下文字符重复扫描前缀，不为每个字符分配字符串，也不拼接中间字符串。跨分配保根接收者，成功后才写目标寄存器，包括源与目标寄存器别名的情况。

大小溢出或结果超出堆可表示存储范围时走现有可恢复 OutOfMemory 路径。分配失败不能修改接收者或发布部分初始化字符串。输入验证保留在已有外部边界，内部转换不重复完整 UTF-8 校验。Unicode 表与算法的不变量属于内部开发契约，不能以用户可触发的致命检查代替错误处理。

## 集成与验证

前端、IR、VM 与工具共享受检查的 string builtin 签名。方法值、成员补全、悬停、文档、格式化、调试查询和持久化 REPL 沿用现有字符串方法路径。字节码镜像验证 builtin ID 和操作数，缓存身份包含生成器与固定输入清单。

`DEPS` 通过 [`tools/unicode_case.py`](../../tools/unicode_case.py) 同步经过内容校验的 UCD 输入。GN 为各输出目录离线生成 `gen/runtime/unicode_case_data.h`。[输入清单](../../third_party/unicode_data/manifest.json)记录输入身份和 Unicode 数据许可证。[`examples/string_case.vela`](../../examples/string_case.vela) 演示公开 API。

测试覆盖 ASCII、空串/已转换文本、无大小写文字、sharp s 展开、带点 I 展开、sigma 的词尾/非词尾位置及 Case_Ignorable 上下文、补充平面映射、组合序列、NUL、不规范化、幂等性、接收者不变和错误实参诊断。Unicode 测试向量来自固定映射数据，不依赖宿主 locale。验证包含确定性重新生成、版本一致、完整映射覆盖、上下文属性范围、分配/GC、VM 寄存器别名、强类型容器、新 Isolate 镜像和 REPL 复用。

Release benchmark 在三种输入规模下测量无需修改的 ASCII、有修改的 ASCII、非 ASCII 映射、展开映射和上下文字符序列。[基准指南](../../benchmarks/README.md)定义测量边界及保留结果。

已记录的 Windows x64 / Ryzen 9 7900X Release 测量中，16,384 字节无需修改的 ASCII 为 302 µs（首次调用 GC 字节为零），有修改的 ASCII 为 582 µs，简单 Unicode 映射为 311 µs。20,480 字节上下文用例为 612 µs；8,192 字节展开用例产生 24,576 字节，耗时 81 µs。从 256 到 4096 输入单元，耗时随规模近似增长。测量包含 VM 执行和 GC，计数不包含原生分配器开销；所有用例完整回收后额外保留堆字节为零。确切中位数、波动、规模和机器/工具链配置见[保留结果](../../benchmarks/results/rolling_summary.json)。

## 参考

- [Dart toLowerCase](https://api.dart.dev/dart-core/String/toLowerCase.html) 与 [toUpperCase](https://api.dart.dev/dart-core/String/toUpperCase.html)。
- [Unicode 默认大小写转换](https://www.unicode.org/versions/latest/core-spec/chapter-3/)与 [SpecialCasing 数据格式](https://www.unicode.org/Public/UCD/latest/ucd/SpecialCasing.txt)；实现测试使用固定版本，不使用这些会变化的参考 URL 作为数据依赖。
- [utf8proc 公开 API](../../third_party/utf8proc/utf8proc.h)与 [Unicode 数据生成输入](../../third_party/utf8proc/data/Makefile)。
- Vela [字符串](strings.md)、[正则表达式](regexp.md)与[开发流程](../../.agents/development_workflow.md)。
