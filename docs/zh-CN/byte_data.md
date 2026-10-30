# 字节存储与带类型视图

[English](../en/byte_data.md) | [**简体中文**](byte_data.md)

`ByteArray` 拥有固定长度的连续字节存储；`ByteData` 是该存储上的带类型读写视图，并负责保活所有者。多字节访问器和显式字节序采用 Dart 风格，保留 Vela 的 ByteArray 命名。不引入 ByteBuffer，也不依赖 [JSON API](json.md)。

```vela
func main(): void {
  const bytes = ByteArray(length: 8);
  const data = ByteData.view(bytes);

  data.setInt32(0, 42, endian: Endian.little);
  const result: int = data.getInt32(0, endian: Endian.little);
  print(result);

  const copy = bytes.slice();
  copy[0] = 0;
  print(bytes[0]); // 42：slice 具有独立存储。
}
```

第一版只处理内存，不加入可变长度字节数组、可分离缓冲区、原生指针包装、共享内存并发、编码 codec、文件、流或网络 API。ByteData 不支持数组下标语法。

## ByteArray API

| API | 契约 |
| --- | --- |
| `ByteArray(length: int)` | 分配指定长度的字节存储，全部初始化为零 |
| `ByteArray.from(values: int[]): ByteArray` | 验证整数元素并复制到新的字节数组 |
| `bytes.length: int` | 只读字节长度，O(1) |
| `bytes[index]: int` | 将一个无符号字节读为 int，O(1) |
| `bytes[index] = value` | 检查边界及 `0 <= value <= 255` 后写入一个字节 |
| `bytes.slice(start?: int, end?: int): ByteArray` | 将选中范围复制到独立存储 |

ByteArray 是专用引用类型，不是 `int[]` 的别名或子类型。每个元素占一个字节，不使用整数或 Value 槽位。所有者的整个生命周期内长度固定，即使没有视图也不允许改变。没有可写 length、capacity、push、pop、reserve 或其它调整大小的操作。允许长度为零；负长度报错；正长度在分配前检查宿主和堆的存储上限，不允许截断或回绕。

下标与元素值均为 int。不执行钳制、取模、数值隐式转换或负下标访问。`from` 拒绝任意越界元素，不采用部分 typed-array API 的整数截断语义。源数组保持不变，不返回部分构造的结果。

`slice` 的端点规则与[普通数组](arrays.md)一致：省略 start 时为零，省略 end 时为 length，负端点从末尾倒数，端点限制在 `[0, length]`。end 小于 start 时返回空数组。复制规范化后的半开范围，即使复制整个数组也不共享存储。极端 int 端点的规范化不能产生溢出。

`from` 显式从 int[] 复制；反向复制需要显式读取下标并写入 int[]，不提供隐式转换、存储强制解释或共享底层缓冲区。`from` 要求静态检查的 int[]，不接受可空整数数组或其它元素类型。本功能不引入 dynamic 或混合容器。

## Endian 与 ByteData API

```vela
enum Endian {
  big,
  little,
  host,
}
```

| API | 契约 |
| --- | --- |
| `ByteData.view(bytes: ByteArray, offset?: int, length?: int): ByteData` | 创建共享视图；offset 默认为零，length 默认为剩余字节数 |
| `data.byteLength: int` | 只读视图字节长度，O(1) |
| `data.getUint8(offset: int): int` | 读取一个无符号字节 |
| `data.setUint8(offset: int, value: int): void` | 检查后写入一个无符号字节 |
| `data.getInt8(offset: int): int` | 读取一个有符号字节 |
| `data.setInt8(offset: int, value: int): void` | 检查后写入一个有符号字节 |

多字节方法使用下列签名模式，后缀与宽度见表：

```text
getXxx(offset: int, endian?: Endian): int
setXxx(offset: int, value: int, endian?: Endian): void

getFloat32(offset: int, endian?: Endian): double
setFloat32(offset: int, value: double, endian?: Endian): void
getFloat64(offset: int, endian?: Endian): double
setFloat64(offset: int, value: double, endian?: Endian): void
```

| 后缀 | 字节宽度 | 整数写入范围／浮点表示 |
| --- | --- | --- |
| `Int8` | 1 | `[-128, 127]`，没有 endian 参数 |
| `Uint8` | 1 | `[0, 255]`，没有 endian 参数 |
| `Int16` | 2 | `[-32768, 32767]` |
| `Uint16` | 2 | `[0, 65535]` |
| `Int32` | 4 | `[-2147483648, 2147483647]` |
| `Uint32` | 4 | `[0, 4294967295]` |
| `Int64` | 8 | 完整有符号 64 位 int 范围 |
| `Float32` | 4 | IEEE 754 binary32，Vela 参数及结果为 double |
| `Float64` | 8 | IEEE 754 binary64 |

所有多字节方法默认使用 `Endian.big`，与 Dart ByteData 一致。`host` 在执行时解析为平台字节序，不保证可移植的外部表示。八位方法没有 endian 参数。不提供 Uint64，因为 Vela int 无法表达其完整范围。

ByteData 不能脱离 ByteArray 独立构造。所有者、基偏移和长度都没有公开 setter。可选参数采用普通省略实参规则，不能用显式 null 代替省略。调用支持 Vela 常规的位置参数与命名参数。

## 视图边界与数值行为

所有者长度为 `N` 时，创建视图要求 `0 <= offset <= N` 且 `0 <= length <= N - offset`。这些边界严格检查，不采用 slice 的规范化或钳制规则。允许在偏移 N 创建空视图。所有偏移均以字节计，各访问器的 offset 相对当前视图。

宽度为 W 的访问先检查 offset 非负、`W <= byteLength` 和 `offset <= byteLength - W`。这与 `offset + W <= byteLength` 等价，同时避免加法溢出。检查成功后才计算所有者中的地址。空视图上的全部已定义宽度访问都失败。

读写允许非对齐偏移。整数按对应方法采用无符号或二进制补码有符号解释。setter 拒绝越界整数，不截断或回绕。所有边界与数值检查必须在修改首字节前完成，失败后全部共享存储保持不变。

Float32 写入将 binary64 按 IEEE 754 最近值、正中取偶数规则转换为 binary32，读取将 binary32 扩展为 double。有限值溢出可以产生无穷大，次正规数与下溢遵循 IEEE 754。Float64 按 binary64 读写。两种宽度都允许 NaN、无穷大和带符号零，不保证 NaN payload 身份。这些二进制操作不采用 JSON 的有限数值限制，int/double 实参转换仍须显式执行。

C++ 实现使用字节复制、无符号组装和保持位表示的转换，不构造非对齐的带类型指针，不违反别名规则，不使有符号算术溢出，也不依赖越界有符号转换。

## 所有权、修改与 GC

创建 ByteData 视图为 O(1)，不复制字节。视图保存 ByteArray 所有者的强引用，以及不可变字节偏移和长度，不能只保存原始地址。即使原始变量离开作用域，存活视图仍保活完整所有者分配；空视图遵循相同规则。

重叠视图在所属 Isolate 线程上立即观察彼此的写入。通过 ByteArray 或视图执行的写入访问同一份存储。`slice` 始终创建独立所有者。不增加原子访问或宿主线程并发修改契约。

ByteArray 对其底层分配负责且仅释放一次；对象大小和可能的外部存储都计入堆核算与 GC 压力。GC 将原始字节视为非引用数据，不扫描其中的位模式寻找指针。ByteData 的追踪访问所有者，引用发布使用现有写屏障。构造和复制在分配安全点前保根所有者与新对象。回收单个视图不能释放仍被存活 ByteArray 或其它视图持有的存储。

现有非移动 GC 和固定长度允许存储地址稳定，但地址稳定不能替代保根。第一版不增加公开原生借用或指针 API。

## 错误与类型规则

| 失败 | 运行时状态／行为 |
| --- | --- |
| 非法 ByteArray 下标、视图范围或访问器范围 | 现有 `IndexOutOfBounds` |
| 负分配长度或越界整数值 | 现有 `InvalidNumericConversion` |
| 分配或可表示存储资源耗尽 | 现有可恢复 `OutOfMemory` |
| 静态实参、下标、值或字节序类型错误 | 编译诊断 |
| 受检查 builtin 的运行时操作数类型错误 | 现有 TypeError 路径 |

表中状态名标识现有执行路径；Error.code 继续使用既有状态到字符串的映射。错误经普通 VM 异常机制捕获，错误对象分配失败时沿用既有处理。非法操作不能返回零或静默忽略写入。

ByteArray 和 ByteData 支持普通引用、可空标注、参数、返回值、字段、捕获和带类型容器。其运行时身份与 int[]、普通实例及彼此保持独立。仅 ByteArray 提供下标访问。`length` 和 `byteLength` 为只读 builtin 属性，成员访问不能伪造字段或绕过检查。Endian 是枚举，不是可替换为整数的选择值。

## 集成与验证

ByteArray 与 ByteData 具有专用前端类型和运行时对象种类，复用现有对象型 Value 表示。受保护的标准环境提供工厂命名空间和 Endian 枚举；构造、属性和方法签名通过 builtin 元数据与补全、悬停、命名实参及源码工具共享。类型别名、可空类型、字段、捕获、数组、集合和 Future 载荷沿用正常类型规则。

实例方法值绑定并通过 GC 保活接收者。没有函数类型上下文时采用完整位置参数签名，包括 Endian；指定 `(int) => int` 等上下文可以选择带默认大端的 getInt32。静态工厂函数值采用完整固定位置参数签名。显式 null 不能代替省略的 int 或 Endian 实参。

每个所有者由一次 GC 分配承载对象头和恰好 length 个原始字节，不复用 int[] 布局或可增长数组的容量机制。当前堆的单次分配大小用 uint32 表示，包含对象头；超出该存储上限的请求走 OutOfMemory。视图存储所有者强引用、基偏移和固定长度，在既有 GC 发布屏障前初始化引用。ByteArray 是 GC 叶对象，ByteData 追踪其所有者；存储计入正常堆核算，在所有者不可达后释放一次。

编译器将字节下标和多字节访问降低为专用的受检查 builtin，普通数组下标保持独立字节码路径。读取、写入、失败与分配沿用现有 IR 效果及异常路径，保证别名可观察性。默认值在按源码顺序求值实参前填充。空安全访问在接收者为 null 时跳过下标和实参，操作成功后才发布结果，包括字节码目标与输入寄存器别名的情况。

版本 7 字节码镜像在强类型数组、集合和运行时类型检查中保留新对象种类约束，并验证 builtin 参数窗口。镜像仅保存元数据与可执行构造操作，不保存存活字节、所有者或原始地址。构建身份包含实现，拒绝旧版本镜像。REPL 提交通过正常 Isolate 所有权保留受检查类型和布局；调试器显示 ByteArray 长度和 ByteData byteLength。runtime 不依赖 compiler 或 tooling。

回归覆盖零初始化、空所有者/视图、复制及 slice 规范化、只读属性、拒绝调整大小、全部整数宽度及端点、大端/小端/宿主字节序、非对齐访问、视图相对边界与极端偏移、失败后存储不变、IEEE 参考向量、带符号零、NaN/无穷大和重叠视图。Float32 在改变宿主舍入模式后仍验证最近偶数舍入，且不修改该环境。GC 测试仅保留普通或空视图并验证最终存储释放。编译器、无源码的新 Isolate 镜像、异步挂起、强类型容器、REPL、LSP、格式化及文档生成均沿用生产路径。

公开 API 示例见 [`examples/byte_data.vela`](../../examples/byte_data.vela)。Release benchmark 测量清零与分配、下标访问、复制、对齐/非对齐带类型访问、字节序、视图构造及 GC 压力。参见[测量边界](../../benchmarks/README.md)与[保留汇总结果](../../benchmarks/results/rolling_summary.json)。

已记录的 Windows x64 / Ryzen 9 7900X Release 测量中，经 VM 分配并清零 4 KiB 的中位数为 303 ns；分配源存储并复制其 4 KiB slice 为 423 ns。只测运行时视图构造时，64 字节到 64 KiB 的所有者均为 13.4–13.5 ns。各字节序、对齐及非对齐的带类型访问用例执行 1,000 对读写，耗时 64.9–68.1 µs。全部 19 项用例通过正确性检查，GC 后额外保留堆字节均为零。五项既有 VM 用例的最新/基线中位数比为 0.992–1.012，数组分配为 1.004。确切波动、计数、源码指纹和计时边界保留在前述汇总结果中；这些本机测量不构成 API 延迟保证。

## 参考

- [Dart Uint8List](https://api.dart.dev/dart-typed_data/Uint8List-class.html)、[ByteData](https://api.dart.dev/dart-typed_data/ByteData-class.html) 与 [Endian](https://api.dart.dev/dart-typed_data/Endian-class.html)。
- [JavaScript Uint8Array](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Uint8Array) 与 [DataView](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/DataView)。
- Vela [数组](arrays.md)、[垃圾回收](garbage_collection.md)与[编译缓存](compilation_cache.md)。
