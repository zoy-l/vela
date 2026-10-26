# 字节存储与带类型视图

[English](../en/byte_data.md) | [**简体中文**](byte_data.md)

> 状态：设计规格。ByteArray、ByteData 和 Endian 尚未实现。实现必须满足下述所有权、检查、编译链路与验证契约。

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

`from` 显式从 int[] 复制；反向复制需要显式读取下标并写入 int[]，不提供隐式转换、存储强制解释或共享底层缓冲区。通过 dynamic 调用 `from` 时，真实容器类型仍须满足 int[]；检查 dynamic[] 的内容属于单独的类型转换。

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
| 通过 dynamic 传入错误实际类型 | 现有受检查转换或类型错误路径 |

表中状态名标识现有执行路径；Error.code 继续使用既有状态到字符串的映射。错误经普通 VM 异常机制捕获，错误对象分配失败时沿用既有处理。非法操作不能返回零或静默忽略写入。

ByteArray 和 ByteData 支持普通引用、可空标注、参数、返回值、字段、捕获和带类型容器。其运行时身份与 int[]、普通实例及彼此保持独立。仅 ByteArray 提供下标访问。`length` 和 `byteLength` 为只读 builtin 属性，成员访问不能伪造字段或绕过检查。Endian 是枚举，不是可替换为整数的选择值。

## 实现范围与完成条件

复用现有[标准环境](../../compiler/frontend/standard_environment.cc)、[builtin 签名接口](../../compiler/frontend/builtins.h)、[运行时对象模型](../../runtime/object.h)和[堆](../../runtime/heap.h)。ByteArray 与 ByteData 需要专用前端类型和运行时对象种类，可以使用现有对象型 Value 表示。不能复用 int[] 布局或可增长数组的重新分配机制来承载字节存储。

| 阶段 | 必须完成的工作 |
| --- | --- |
| 标准声明 | 注册 ByteArray、ByteData、Endian 及声明依赖闭包，向源码检查和工具提供准确签名与默认值 |
| 前端 | 增加专用类型、构造、字节下标、只读属性、方法和枚举检查；保留可空与容器不变性规则 |
| 运行时 | 接通原始固定存储、保活所有者的视图、字节及带类型检查读写、GC 追踪、屏障、核算与销毁 |
| IR 与编译器 | 按准确宽度和类型降低字节及视图操作，标记分配、读取、别名写入和可抛出效果 |
| VM 与 builtin | 接通全部构造、访问器、默认值、字节序和现有错误，保证全部别名可观察修改 |
| 字节码镜像与缓存 | 编码类型、布局与 builtin 元数据，校验参数数量与操作数范围，按需更新格式和构建身份；不保存运行时所有者或指针 |
| 工具链 | 接通签名、命名实参、悬停、文档、格式化、调试表示及持久化 REPL 的类型和布局保留 |
| 交付 | 增加 `examples/byte_data.vela`，全部契约完成后才标记功能已实现 |

读取不能跨越别名写入被当作常量；不能仅因为某个视图自身已无引用就删除其写入。边界、数值失败和分配都是可观察效果。编译与编辑器消费者共享 builtin/签名数据，不维护第二份 API 模型。runtime 保持独立于 compiler 和 tooling。

必要测试覆盖零初始化、空所有者和视图、显式复制、slice 规范化、只读属性、拒绝调整大小、字节值范围、全部整数宽度和端点、默认/大端/小端/宿主字节序、非对齐访问、视图相对边界、负数与极端 int 偏移、失败后字节不变、浮点参考向量、负零和非有限值及重叠视图。GC 测试必须覆盖只保留视图、复制或视图构造后触发收集、从动态及静态容器追踪所有者，以及全部根消失后的存储释放和核算。编译器、工具及新 Isolate 镜像测试必须覆盖两种引用类型与 Endian。

实现完成边界执行相关 Debug/Release 回归、工具测试和仓库静态检查。Release benchmark 测量分配与清零、字节访问、复制吞吐、对齐及非对齐带类型访问、字节序转换、视图创建和 GC 压力。本文不声称这些检查或基准已经通过。

## 参考

- [Dart Uint8List](https://api.dart.dev/dart-typed_data/Uint8List-class.html)、[ByteData](https://api.dart.dev/dart-typed_data/ByteData-class.html) 与 [Endian](https://api.dart.dev/dart-typed_data/Endian-class.html)。
- [JavaScript Uint8Array](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Uint8Array) 与 [DataView](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/DataView)。
- Vela [数组](arrays.md)、[垃圾回收](garbage_collection.md)与[编译缓存](compilation_cache.md)。
