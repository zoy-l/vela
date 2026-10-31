# 字符串数值解析

[English](../en/numeric_parse.md) | [**简体中文**](numeric_parse.md)

`int.parse`、`int.tryParse`、`double.parse` 和 `double.tryParse` 将完整字符串转换为原生数值。
接口采用 Dart 风格，整数始终遵守 Vela 的有符号 64 位范围，不依赖宿主 locale。

```vela
func main(): void {
  print(int.parse('42'));
  print(int.parse('ff', radix: 16));
  const missing: int? = int.tryParse('unknown');
  print(missing ?? 0);
  print(double.parse('1.25e2'));
}
```

| API | 结果 |
| --- | --- |
| `int.parse(source: string, radix?: int?): int` | 解析成功返回 int，失败抛出 Error |
| `int.tryParse(source: string, radix?: int?): int?` | 无效文本或整数越界时返回 null |
| `double.parse(source: string): double` | 解析成功返回 binary64，失败抛出 Error |
| `double.tryParse(source: string): double?` | 无效文本返回 null；NaN 是有效结果 |

这些静态方法无需 import。类型别名、位置和命名实参、类型检查、文档与编辑器使用相同签名。
局部同名变量遵循普通遮蔽规则。方法函数值使用固定位置参数签名；例如 int.parse 的函数值为
`(string, int?) => int`，自动进制通过第二个参数 null 表示。解析不会隐式接受数值或 null source。

两类解析都只忽略首尾的 ASCII 空白：空格以及 `\t`、`\n`、`\v`、`\f`、`\r`。
不忽略其它 Unicode 空白。去除空白后必须完整匹配输入；不接受前缀解析、内部空白、分隔下划线、
非 ASCII 数字、内嵌 NUL 或尾随文本。空串与仅含空白的输入无效。

## 整数

文本允许一个前导 `+` 或 `-`。省略 radix 或传入 null 时，默认采用十进制；符号之后的 `0x`
或 `0X` 前缀选择十六进制。显式 radix 必须为 2..36，字母数字不区分大小写；显式进制不特殊识别
`0x`，每个字符都必须是该进制的合法数字：radix 16 拒绝 `0xff`，radix 36 则将 `0x` 当作数字文本。
不自动识别二进制或八进制前缀。前导零不改变进制。

所有结果位于 `[-9223372036854775808, 9223372036854775807]`。解析在乘加前检查溢出，
不截断、不回绕，也不将十六进制无符号值重解释为负数。例如 `0xffffffffffffffff` 越界。
整数 `-0` 返回零。小数点或指数不是整数语法。

## 浮点数

十进制语法为可选符号、尾数和可选的 `e` / `E` 指数。尾数至少包含一个数字，可以省略小数点
一侧的数字，例如 `.5` 和 `1.`；指数可带符号，但必须有数字。接受区分大小写的 `NaN` 和
`Infinity`，两者均可带一个前导符号。不接受十六进制浮点数、`inf`、小写 `nan` 或 NaN 载荷语法。

使用既有 Abseil 数值转换实现 IEEE 754 舍入。支持次正规数及带符号零；有限十进制文本溢出时
产生相应符号的 Infinity，下溢时舍入为次正规数或带符号零。这些是有效解析结果，tryParse 也会
返回它们。NaN 的载荷位不构成契约；需要有限数值时显式使用 Math.isFinite。

## 错误与生命周期

| 失败 | 契约 |
| --- | --- |
| int.parse 文本无效或越界 | `Error(code: 'IntParseError')` |
| double.parse 文本无效 | `Error(code: 'DoubleParseError')` |
| 显式 radix 不在 2..36 | `Error(code: 'InvalidRadix')`，tryParse 也抛出 |
| 静态实参类型、名称或数量错误 | 前端诊断 |
| 错误对象分配失败 | 现有 OutOfMemory 路径 |

失败可以由普通 catch/finally 捕获，保留调用栈；消息文案不是稳定标识。tryParse 只将无效文本
转换为 null，不吞掉实参求值错误或非法进制。成功结果是原生 int/double，不分配数值包装对象，
也不修改或保留 source。标准方法通过 builtin 链路运行，声明和实现均参与编译缓存身份。

示例见 [numeric_parse.vela](../../examples/numeric_parse.vela)。

Release 基准通过原生执行测量十进制/十六进制 int64、double 和无效 tryParse 路径。
首次调用堆分配、完整 GC 后保留量与时间分别报告。
参见[测量边界](../../benchmarks/README.md)与[汇总证据](../../benchmarks/results/rolling_summary.json)。
