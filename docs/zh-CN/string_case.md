# 字符串大小写转换

[English](../en/string_case.md) | [**简体中文**](string_case.md)

> 状态：设计规格。下述两个大小写转换方法尚未实现。已有字符串 API 见[字符串系统](strings.md)。

Vela 为不可变 `string` 增加 Dart 风格的实例方法 `toLowerCase()` 和 `toUpperCase()`。二者采用与 locale 无关的 Unicode 默认大小写转换，覆盖多码点映射及与语言无关的上下文规则。返回字符串，不修改接收者。

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

接收者必须为非空 string；可空接收者使用已有空安全访问或类型收窄。方法均不接受参数。错误静态接收者或实参由前端诊断，动态调用执行相同运行时检查。结果是普通不可变 UTF-8 字符串，可用于字段、数组、捕获和原生字符串调用。

采用固定 Unicode 数据版本的默认转换：无条件完整映射，加上与语言无关的上下文映射，包括 Final_Sigma。不应用土耳其语、阿塞拜疆语、立陶宛语等 locale 特定规则。默认情况下，I 转小写为 i，带点大写 I 转为 i 加组合点。

单个输入码点可以产生多个输出码点；输出字节数和码点数都可能改变，转换不保留原始索引。空输入仍为空；没有映射的无大小写字符、补充平面字符、组合标记、emoji 和内嵌 NUL 保留。上下文规则检查 Unicode Cased 和 Case_Ignorable 属性，不能使用仅针对 ASCII 的词边界。

不做规范化或去重音。完整转换可以产生分解序列，必须保留该序列。大小写转换不是可逆编码，大写后再小写不保证恢复原文。固定映射数据下，重复执行同一种转换应保持幂等。如果内容没有变化，可以复用原不可变字符串；对象分配身份不属于 API 契约。

RegExp 忽略大小写匹配不能先用这些方法转换整份输入：展开会改变偏移，也不能实现 PCRE2 的 caseless 匹配规则。

## Unicode 数据与实现

复用现有 utf8proc 依赖提供简单映射，增加生成的 Unicode 完整/上下文映射数据。大小写操作独立于正则引擎。现有 [Unicode 操作](../../runtime/unicode_operations.cc)继续通过 utf8proc 提供码点与字素簇分割。

补充数据必须与固定 utf8proc 构建采用相同 Unicode 版本：从 SpecialCasing.txt 提取无条件及与语言无关的条件条目，从 DerivedCoreProperties.txt 提取 Cased 和 Case_Ignorable 范围。存在完整/上下文条目时使用对应规则，其余码点使用 utf8proc 的公开简单映射函数。Final_Sigma 根据原始输入上下文计算。默认大小写契约排除带 locale 标签的映射，仅逐码点处理不能实现完整转换。

通过仓库依赖流程管理不可变、校验内容身份的 UCD 源输入，提供使用项目 Python 工具链的确定性离线生成器并接入 GN。生成不能依赖宿主 Unicode 数据库、会变化的 latest URL 或构建时网络。不读取 utf8proc 私有序列表，不修改上游源码。数据/schema 校验及重新生成检查由生成器承担，不在每次运行时转换中重复执行。

在既有依赖/构建记录中登记 UCD release、输入身份、生成输出、生成器和 Unicode 数据许可证，要求补充数据与 utf8proc 的 Unicode 版本一致。PCRE2 的 Unicode 表独立固定，不要求与之相同，因为它负责正则匹配而非本转换 API。Unicode 更新属于可观察变化，计入构建/缓存身份并审查测试向量。不按宿主 OS、浏览器或 locale 选择映射，不在运行时下载数据。宿主 C/C++ locale 函数或单独使用 utf8proc 简单映射均不能替代完整算法。

对已经验证的运行时字符串复用 [UTF-8 基础操作](../../base/utf8.h)，以线性扫描计算映射、上下文和受检查的输出大小，随后只分配一次最终堆字符串并直接写入合法 UTF-8。不为每个上下文字符重复扫描前缀，不为每个字符分配字符串，也不拼接中间字符串。跨分配保根接收者，成功后才写目标寄存器，包括源与目标寄存器别名的情况。

大小溢出或结果超出堆可表示存储范围时走现有可恢复 OutOfMemory 路径。分配失败不能修改接收者或发布部分初始化字符串。输入验证保留在已有外部边界，内部转换不重复完整 UTF-8 校验。Unicode 表与算法的不变量属于内部开发契约，不能以用户可触发的致命检查代替错误处理。

## 实现范围与完成条件

| 阶段 | 必须完成的工作 |
| --- | --- |
| 依赖与数据 | 复用固定 utf8proc；通过依赖/GN 流程管理同版本 UCD 输入、Unicode 许可证、确定性生成及完整/上下文表 |
| 前端与签名 | 将两个方法注册为 string builtin，编辑器和文档消费者共享无参数、返回 string 的签名 |
| 运行时 | 实现完整映射、上下文转换、输出大小计算、分配/保根及普通分配错误 |
| IR 与 VM | 接通两个 builtin ID、分配/可抛出效果、源码求值顺序及寄存器别名安全 |
| 镜像与缓存 | 验证新 builtin ID 与操作数，代码和 Unicode 数据计入构建/缓存身份，必要时更新镜像版本 |
| 工具链 | 接通成员补全、悬停、诊断、文档生成、格式化、调试求值和持久化 REPL |
| 交付 | 增加 `examples/string_case.vela`，全部路径与必要检查完成后才勾选路线图 |

必要测试覆盖 ASCII、空串/已转换文本、无大小写文字、sharp s 展开、带点 I 展开、sigma 的词尾/非词尾位置及 Case_Ignorable 上下文、补充平面映射、组合序列、NUL、不规范化、幂等性、接收者不变和错误实参诊断。Unicode 测试向量来自固定映射数据，不依赖宿主 locale。验证确定性重新生成、版本一致、完整映射覆盖与上下文属性范围。施压分配、输出大小检查、转换期间 GC、VM 寄存器别名、强类型容器、新 Isolate 镜像和 REPL 复用。

实现完成边界执行相关 Debug/Release 回归、工具测试和仓库静态检查。Release benchmark 测量无需修改的 ASCII、有修改的 ASCII、非 ASCII 映射、展开映射和上下文字符序列，报告分配、输出字节数及随输入大小的变化。本文不声称已有实现或性能结果。

## 参考

- [Dart toLowerCase](https://api.dart.dev/dart-core/String/toLowerCase.html) 与 [toUpperCase](https://api.dart.dev/dart-core/String/toUpperCase.html)。
- [Unicode 默认大小写转换](https://www.unicode.org/versions/latest/core-spec/chapter-3/)与 [SpecialCasing 数据格式](https://www.unicode.org/Public/UCD/latest/ucd/SpecialCasing.txt)；实现测试使用固定版本，不使用这些会变化的参考 URL 作为数据依赖。
- [utf8proc 公开 API](../../third_party/utf8proc/utf8proc.h)与 [Unicode 数据生成输入](../../third_party/utf8proc/data/Makefile)。
- Vela [字符串](strings.md)、[正则表达式](regexp.md)与[开发流程](../../.agents/development_workflow.md)。
