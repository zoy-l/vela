# UTC 日期与时间 API

[English](../en/time.md) | [**简体中文**](time.md)

Vela 用有符号 int 表示 Unix 毫秒时间戳。Date 静态类提供四个转换方法，通过只读 DateParts 返回 UTC 日历快照。日历转换复用仓库构建源码清单中已有的 Abseil Time 实现。

```vela
func main(): void {
  const timestamp: int = Date.parse('2026-10-26T12:30:45.123+08:00');
  print(Date.toISOString(timestamp)); // 2026-10-26T04:30:45.123Z

  const parts: DateParts = Date.toUTC(timestamp);
  const restored: int = Date.fromUTC(
    year: parts.year,
    month: parts.month,
    day: parts.day,
    hour: parts.hour,
    minute: parts.minute,
    second: parts.second,
    millisecond: parts.millisecond,
  );
  print(restored == timestamp); // true
}
```

第一版覆盖 UTC 日历转换，以及带明确数字偏移的常用 ISO 8601 / RFC 3339 日期时间严格子集。不增加 DateTime 或 Duration 类型、系统本地时间转换、命名时区、夏令时规则、本地化格式、任意格式串或日历运算。定时器与调度属于独立的[异步契约](async_tasks.md)。此 API 不依赖 dynamic、JSON 或正则表达式。

## 公开 API

| API | 契约 |
| --- | --- |
| `Date.now(): int` | 已有墙钟 Unix 毫秒时间戳 |
| `performance.now(): double` | 已有单调时钟毫秒读数，通过读数相减测量耗时 |
| `Date.parse(source: string): int` | 解析支持的显式偏移日期时间，返回 Unix 毫秒 |
| `Date.toISOString(timestamp: int): string` | 将范围内时间戳输出为规范 UTC 文本 |
| `Date.fromUTC(year: int, month: int, day: int, hour?: int, minute?: int, second?: int, millisecond?: int): int` | 验证 UTC 字段并构造 Unix 毫秒，省略的时间字段均为零 |
| `Date.toUTC(timestamp: int): DateParts` | 返回新的只读 UTC 日历快照 |

Date 保持为不可构造的标准静态类。Date 与 DateParts 无需 import，遵循既有名称预留和局部遮蔽规则。Date 声明引用 DateParts 时，标准环境必须带入该依赖，即使用户没有写出 DateParts 名称。内部标准源码参与 overlay 保护与编译缓存指纹。

可选参数表示省略，不表示 null。调用保留普通源码求值顺序及位置/命名实参检查，包括省略前序可选字段后用名称传入时间字段。不从当前日期、宿主时区或前一次调用推断参数。全部数字参数和字段均为 int，int/double 转换仍须显式执行。

现有时钟契约保持不变：校时可能使 Date.now 前进或后退；performance.now 起点未定义，用于计算耗时。连续读数相同是合法情况。单调读数不是 Unix 时间戳，不能用于日历转换。

## 日历、精度与支持范围

使用向前延伸的公历，年份 1..9999，月份和日期从一开始计数。year 为 99 就表示 0099，不表示 1999。整个范围沿用公历闰年规则。每个 Unix 日为 86,400 秒，不表示闰秒。

| 字段 | 合法范围 |
| --- | --- |
| `year` | 1..9999 |
| `month` | 1..12 |
| `day` | 1..该年该月的真实天数 |
| `hour` | 0..23 |
| `minute` | 0..59 |
| `second` | 0..59 |
| `millisecond` | 0..999 |

字段不能自动进位：month 13、2 月 30 日、hour 24、second 60、负字段和 millisecond 1000 均报错。将 int 参数缩窄为后端字段类型前先检查，不能利用 Abseil 日历归一化静默修改无效日期。

四个转换方法支持闭区间 `[-62135596800000, 253402300799999]` 的时间戳，对应 `0001-01-01T00:00:00.000Z` 至 `9999-12-31T23:59:59.999Z`。应用输入偏移后，归一化的 UTC 时刻也必须位于此范围。现有 Date.now 仍返回 int 墙钟读数，不将这个日历范围施加给宿主时钟绑定。

支持负时间戳。Date.toUTC(-1) 对应 `1969-12-31T23:59:59.999Z`，子秒字段必须按向下取整的日历分解计算，不能直接使用负余数。普通 int 运算不会自动检查日历范围，每个转换方法在自身输入边界检查。

## 解析与规范输出

严格接受以下 ASCII 形式：

```text
YYYY-MM-DDTHH:mm:ssZ
YYYY-MM-DDTHH:mm:ss.SZ
YYYY-MM-DDTHH:mm:ss.SSZ
YYYY-MM-DDTHH:mm:ss.SSSZ
```

末尾 Z 均可替换为数字偏移 `+HH:mm` 或 `-HH:mm`。T 和 Z 必须大写，日期时间字段为固定宽度。偏移小时为 00..23，分钟为 00..59；换算 UTC 时减去正偏移。Z 与 +00:00 均表示 UTC；拒绝 -00:00，因为 RFC 3339 用它表示未知本地偏移。偏移语法不加载或保留命名时区。

小数秒可省略；存在时必须为一至三位数字，右侧补零至毫秒。超过三位即拒绝，即使额外位全为零。不通过截断或舍入丢失输入精度。

拒绝省略偏移、仅日期或仅时间、首尾空白、小写分隔符、扩展/带符号/零年份、星期日期与年内日序日期、逗号小数、闰秒、24:00、偏移秒数、无穷时间、内嵌 NUL、尾随内容和非 ASCII 数字。日历字段同时满足前述范围规则。这是明确的子集，不承诺完整 ISO 8601 或 JavaScript Date.parse 习惯。

Date.toISOString 始终返回 24 字符 ASCII 形式 `YYYY-MM-DDTHH:mm:ss.SSSZ`，保留全部三位小数和四位年份，不依赖 locale 或机器时区。对于范围内时间戳 t，`Date.parse(Date.toISOString(t)) == t`；将 Date.toUTC(t) 的全部字段传回 Date.fromUTC 也得到 t。格式化解析结果时规范化偏移与精度，不保留输入原文。

## DateParts

| 只读属性 | 类型／值 |
| --- | --- |
| `parts.year: int` | UTC 年份 |
| `parts.month: int` | UTC 月份，1..12 |
| `parts.day: int` | UTC 月内日期 |
| `parts.hour: int` | UTC 小时 |
| `parts.minute: int` | UTC 分钟 |
| `parts.second: int` | UTC 秒 |
| `parts.millisecond: int` | 该秒内的毫秒，0..999 |

DateParts 是含七个 int 字段的固定布局标准类，不是 Map 或时刻对象。构造器不公开，Date.toUTC 只发布完整初始化的快照。Vela 代码不能修改属性。支持普通引用、可空标注、字段、捕获和强类型容器，使用正常类/GC 模型。每次 toUTC 返回新实例，相等性沿用普通引用身份规则。

不隐式转换为 string、时间戳、Map 或 JSON。持久化时刻时显式选择时间戳或 Date.toISOString 的结果。DateParts 不保留时区、原生指针、解析缓冲区或后端日历对象引用。

## 错误与分配

| 失败 | 契约 |
| --- | --- |
| 非法解析语法、字段、日期、精度或偏移 | code 为 `DateParseError` 的 Error，消息给出明确原因 |
| 非法 fromUTC 字段、越界时间戳或归一化 UTC 结果 | code 为 `DateRangeError` 的 Error，指出字段或支持范围 |
| 静态实参类型错误、构造 DateParts 或修改属性 | 前端诊断，受检查的 builtin 调用也验证运行时操作数 |
| 结果或错误对象分配失败 | 现有可恢复 OutOfMemory 路径 |

输入日历字段合法且位于范围内，但偏移使 UTC 结果越界时，parse 产生 DateRangeError；输入字段本身非法或越界时产生 DateParseError。错误可经普通 VM catch/finally 捕获，保留源码调用栈，不能转为 NaN、null、零、被修正的日期或通用原生调用失败。可读消息的具体措辞不构成程序契约，Error.code 才是稳定标识。

解析和字段构造返回整数，不需要结果包装。格式化返回普通不可变字符串，toUTC 分配普通类实例。分配安全点期间保根源字符串、实参及新结果，成功后才发布目标。失败不暴露部分字符串或对象，也不修改输入。

## 后端与集成

复用 [Abseil Time](../../third_party/abseil-cpp/absl/time/time.h)，相关源码已位于[受管理构建清单](../../third_party/abseil_sources.gni)，包括 UTC 日历转换与固定数字解析/格式化能力。新增方法不查找时区数据库，不发现本地时区、不查询 locale 或读取时钟，无需增加时间库依赖。

在输入边界约束准确 ASCII 语法和毫秒精度，再使用 Abseil 验证转换。不能因其 ParseTime 接受小写分隔符、任意精度、闰秒归一化或无穷时间而放宽本 API。fromUTC 先检查数字字段范围，再构造后端日历值并比较归一化字段与原请求，拒绝无效日期。将最终时刻转为 int64 毫秒前先检查范围。格式化始终显式指定 UTC，例如内部固定格式 `%E4Y-%m-%dT%H:%M:%E3SZ` 确保规范宽度。子秒字段相对 UTC 日历秒起点计算，包括负时间戳。解析使用显式长度，不依赖 C 字符串终止或正则表达式。

现有[时钟原生绑定](../../runtime/standard_library.cc)保留当前职责。四个新转换方法接入标准 intrinsic/builtin 路径，由 runtime/VM 拥有带类型 DateParts 分配与领域错误。当前[原生 codec](../../runtime/native_function.h)不返回任意类实例，不为这个结果类型引入未检查 Value 返回或通用对象绑定功能。把经过检查的标准 DateParts 类布局传给运行时消费者，并在编译/加载边界验证，不能在使用处猜布局。无需增加原始 Value 种类或新的日历对象表示。

受保护的标准环境提供受检查的签名和 DateParts 依赖闭包。标准静态方法的 intrinsic 标记经特化与 lowering 生成普通 builtin 包装函数。LSP、文档和源码工具消费相同的受检查签名。方法值具有完整的固定参数列表；直接调用时，四个可选时间字段省略后补零。

Isolate 拥有规范 DateParts 布局，编译将受检查的标准声明绑定到该布局。版本 7 镜像编码并验证标准类角色和形状，VM 验证 toUTC 包装函数的类池操作数确实指向该布局。程序和 REPL 提交从所属 Isolate 借用布局。参见[字节码镜像](compilation_cache.md)与可运行的 [`examples/time.vela`](../../examples/time.vela)。

## 验证

必要回归覆盖纪元零点、负一毫秒、最早/最晚时刻及越界一毫秒、低于 100 的年份、月份天数、公历整百闰年规则、全部默认值、命名实参求值顺序和全部字段/类型错误。解析覆盖每种精度、正负偏移及跨日/跨年、归一化后越界、拒绝未知偏移、缺失时区、空白/NUL/尾随文本、过高精度及闰秒。验证两项往返恒等式、DateParts 不可修改与新实例身份、VM 目标别名、GC/保根、领域错误传播与 VM 复用、工具、新 Isolate 镜像和持久化 REPL 生命周期。宿主时区或 locale 改变不能改变结果。

Release benchmark 分别测量解析、规范格式化、UTC 构造/分解、分配及现有时钟调用回归。[基准指南](../../benchmarks/README.md)定义测量边界及保留结果。

已记录的 Windows x64 / Ryzen 9 7900X Release 测量中，稳态中位数为：解析 276 ns、格式化 438 ns、fromUTC 280 ns、toUTC 338 ns，包含 VM 执行。parse/fromUTC 首次调用 GC 字节为零，格式化为 128，toUTC 为 256；这些是计入堆的字节增量，并非分配次数，也不包含后端/原生分配开销。所有用例完整回收后额外保留堆字节为零。另一个配置一致、重复七次的回归组中，墙钟/单调时钟耗时相对基线为 0.929/0.891，三个同步 VM 用例为 0.991..1.022。这些是特定负载的测量，不是延迟保证；确切波动和配置见[保留结果](../../benchmarks/results/rolling_summary.json)。

## 参考

- [现有标准声明](../../compiler/frontend/builtins/standard_environment.cc)与[标准库时间契约](standard_library.md#时间-api)。
- [Abseil 日历类型与归一化](../../third_party/abseil-cpp/absl/time/civil_time.h)。
- [RFC 3339](https://www.rfc-editor.org/rfc/rfc3339)，包括未知本地偏移约定；本 API 仅支持明确列出的子集。
- Vela [异常](exceptions.md)、[源码工具](source_tooling.md)与[编译缓存](compilation_cache.md)。
