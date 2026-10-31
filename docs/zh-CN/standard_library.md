# 标准库与命令行退出规范

[English](../en/standard_library.md) | [**简体中文**](standard_library.md)

`Math`、`Random`、`performance`、`Date`、`DateParts`、`Json` 和 `Error` 是系统默认声明的全局对象，无需显式 import 导入即可使用。原生 C++ 函数调用依然保持静态检查与静态链接。编译产物仅针对实际被引用的标准对象生成声明与原生函数绑定；普通脚本不会被迫加载整个标准库运行时环境。这些全局名称属于保留字，但允许被局部变量遮蔽（shadow）。内部标准库声明的源码参与编译缓存指纹计算，不能通过外部源码覆盖（source overlay）篡改。用户通过 import 仅解析本地文件，旧式的 `std:` 导入前缀已被彻底移除。宿主应用可通过强类型原生注册表提供业务 API；CLI 的系统时钟与 Isolate 局部的随机数实现均作为其默认的宿主绑定提供。

```typescript
const result = Math.sqrt(9.0);
const values: int[] = [3, 1, 2];
values.sort((left, right) => left - right);
const sum = values.reduce((total, value) => total + value, 0);
```

## 标准输出

输出使用全局 `print(value)` 函数，向所属 Isolate 的输出流写入内容并自动追加换行符。字符串插值会自动格式化基本类型值与 null。语言未设立内置的 `Console` 对象，亦不提供内置的控制台输入 API。

宿主应用可调用 `isolate.SetOutput(output)` 传入借用的 C++ 输出流，该流的生命周期必须覆盖 Isolate 的整个执行期。默认输出流为 `std::cout`。

## 时间 API

`performance.now(): double` 读取单调时钟时间（单位：毫秒），保留亚毫秒级小数精度。计算耗时只需将两次读取结果相减；其时间零点未作规定，连续两次读取返回相同数值属于正常合法情况。`Date.now(): int` 读取自 Unix 纪元以来的整数毫秒数；系统时钟被调整时该墙钟时间可能发生跃变。这些名称与单位严格对标 JavaScript 的 [performance.now](https://developer.mozilla.org/en-US/docs/Web/API/Performance/now) 与 [Date.now](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Date/now)。Vela 未引入完整的浏览器 Date 或 Performance 复杂对象模型。旧有的 `Time` 对象与微秒 API 已被废除。数值类型保持严格强类型：与 `performance.now()` 比较时必须使用 `0.0`，或显式进行数值类型转换。

`Date.parse(source)` 与 `Date.toISOString(timestamp)` 转换严格 ISO 日期时间文本和 Unix 毫秒。`Date.fromUTC(year, month, day, ...)` 构造时刻，`Date.toUTC(timestamp)` 返回只读 DateParts 快照。日历转换支持 1..9999 年，始终显式使用 UTC，不受宿主时区影响。DateParts 无需导入，名称保护和局部遮蔽规则与其它标准声明一致。完整语法、默认值、范围和错误码见 [UTC 日期与时间 API](time.md)。

## JSON

`Json.parse(source: string): dynamic` 返回原生基本值、`dynamic[]` 和有序 `Map<string, dynamic>` 对象。`Json.stringify(value: dynamic): string` 输出紧凑 JSON，通过公开 `toJson()` 方法支持业务对象。动态操作和进入具体类型的赋值仍进行检查；数字、Unicode、循环与错误契约见 [JSON 与动态值](json.md)。

## 数值解析

`int.parse` / `int.tryParse` 和 `double.parse` / `double.tryParse` 解析完整字符串，不做隐式转换。非法文本抛出领域 Error，tryParse 则返回 null；显式 radix 非法时仍抛出错误。整数结果必须位于有符号 int64 范围内。详见[字符串数值解析](numeric_parse.md)。

## 字节存储

`ByteArray(length: int)` 分配固定长度、零初始化的原始字节。`ByteArray.from(values: int[])` 与 `bytes.slice(...)` 显式复制存储，`ByteData.view(bytes, ...)` 则保活并共享所有者。带类型访问器检查视图相对边界和整数范围，允许非对齐偏移。多字节方法默认采用 `Endian.big`，小端与宿主字节序通过枚举显式选择。详见[字节存储与带类型视图](byte_data.md)。

## 正则表达式

`RegExp(source, ...)` 编译不可变的 PCRE2 Unicode 模式。搜索方法返回 bool、可空字符串或匹配对象，以及立即求值的带类型匹配数组。`RegExpMatch` 保活输入和模式，暴露码点范围及编号／命名捕获；未参与的捕获返回 null。`RegExp.escape` 生成字面匹配片段。配置、限制与 Error code 详见[正则表达式](regexp.md)。

## 随机数生成

`Random.seed(value: int): void`、`Random.nextInt(bound: int): int` 以及 `Math.random(): double` 采用 Isolate 独立的 xoroshiro128++ 1.0 算法，并使用 SplitMix64 进行状态播种。每个 Isolate 默认以种子 0 初始化，相同的种子与相同的调用序列严格保证可复现完全相同的结果。种子的全部 64 位（包括负整数的二进制位）均得到完好保留。宿主可独立对 `isolate.random()` 播种。这属于仿真模拟生成器，并非密码学安全伪随机数发生器。

其实现严格遵循 David Blackman 与 Sebastiano Vigna 发布的公有领域 [xoroshiro128++ 官方参考实现](https://prng.di.unimi.it/xoroshiro128plusplus.c) 以及 Sebastiano Vigna 的 [SplitMix64 参考实现](https://prng.di.unimi.it/splitmix64.c)。生成器维护 16 字节内部状态并使用无符号算术与循环移位操作。`nextInt` 在 `[0, bound)` 区间内采样，采用拒绝采样法避免模偏差（modulo bias），并拒绝非正整数的 bound 参数。`Math.random` 提取 53 位随机位，返回 `[0, 1)` 区间内的双精度浮点数。单元测试严格验证了独立的参考测试向量、重新播种及极限边界。

## 数学库 Math

所有数学函数的输入参数与数值返回值均为 `double`；在需要的地方请使用显式的 `toDouble()` 或 `toInt()`。`Math` 对象的静态成员包括 `PI`、`E`、`random`、`sqrt`、`log`、`exp`、`sin`、`cos`、`tan`、`floor`、`ceil`、`round`、`trunc`、`abs`、`pow`、`min`、`max`、`clamp`、`isFinite` 以及 `isNaN`。二元数学函数接收 2 个参数；clamp 接收 `(value, lower, upper)` 3 个参数。`round` 采用半数远离零（round-half-away-from-zero）舍入规则。基础超越函数调用平台 C++ 数学库，其最低有效位可能存在微小平台差异。

传入负数的 sqrt、传入非正数的 log 以及无序/包含 NaN 的 clamp 边界会抛出可被捕获的运行时异常。其它定义域越界或数值溢出结果保持 IEEE 标准的 NaN 或正负无穷大，允许脚本通过 `isNaN`/`isFinite` 显式判断。min/max 与 clamp 遵循 fmin/fmax 语义；当恰好有一个操作数为 NaN 时，有效数值优先胜出。不发生任何隐式类型提升。`Math`、`Random`、`performance` 和 `Date` 仅暴露静态接口，不可被实例化构造；数学常量不可在脚本中被赋值修改。宿主显式注册的原生函数优先于默认绑定，且必须匹配其声明的静态签名。

## 集合操作与高阶方法

数组是可动态扩容的强类型 `T[]`，具有紧凑连续的基本类型或引用存储。下列方法完整保留原数组的元素类型与可空契约：

- `slice(start?: int, end?: int): T[]`：浅拷贝 `[start, end)` 范围，默认截取整个数组。
- `fill(value: T, start?: int, end?: int): T[]`：填充指定区间并返回接收者数组自身。
- `reverse(): T[]`：原地反转数组并返回接收者自身。

端点为负数时从末尾倒数计算，并自动限制在 `[0, length]` 范围内。若 end 小于 start 则选取空区间。原地变更加变方法详见[动态可增长数组](arrays.md)。旧有的 `copy`、`insert` 和 `removeAt` 方法已废除；统一使用 `slice()`、`splice()`、`shift()` 与 `unshift()`。

数组拷贝一次性分配目标内存并进行连续数据复制；fill 与 reverse 仅检索一次存储类型并执行针对具体类型的特化循环。保根与写屏障完整覆盖引用类型元素。对包含类对象的数组进行拷贝，新旧数组共享相同的类实例对象，但拥有各自独立的数组存储结构。支持长度为 0 的操作。

数组高阶算法作为实例方法提供。元素类型从接收者数组中提取；`reduce` 从必填的初始值中推导其累加器类型：

- `forEach(callbackFn: (T, int, T[]) => R): void`：忽略回调函数的返回值。
- `some(callbackFn: (T, int, T[]) => bool): bool`：测试是否有元素满足条件。
- `every(callbackFn: (T, int, T[]) => bool): bool`：测试是否所有元素满足条件。
- `findIndex(callbackFn: (T, int, T[]) => bool): int`：返回首个满足条件的元素索引，未找到返回 -1。
- `reduce(callbackFn: (R, T, int, T[]) => R, initialValue: R): R`：归约累加计算。
- `map(callbackFn: (T, int, T[]) => R): R[]`：分配并返回独立的映射结果数组。
- `sort(compareFn?): T[]`：原地稳定排序并返回接收者数组自身。

回调函数可以省略未使用的末尾参数。传入的 index 索引从 0 开始，array 参数为原始接收者数组。与 JavaScript 不同，reduce **始终强制要求**提供初始值，并从索引 0 开始遍历。在空数组上调用 reduce 直接返回该初始值，不触发回调函数。

回调函数使用与普通函数实参完全相同的[上下文类型推导](type_inference.md)：例如 `values.sort((left, right) => left - right)`、`values.some(value => value > 0)` 以及 `values.reduce((sum, value) => sum + value, 0)` 均无需为参数重复书写类型标注。reduce 从 `initialValue` 自动推导累加器类型，即使使用命名实参打乱传参顺序也能精准推导。

`Set<T>.union(other: Set<T>)`、`intersection(other: Set<T>)` 与 `difference(other: Set<T>)` 返回独立新建的 `Set<T>`，保持左操作数的插入顺序（并集后追加右操作数中未出现的新元素）。它们直接遍历内部哈希表，无需分配临时的键数组。

回调函数严格按遍历顺序求值；some、every 与 findIndex 一旦得出确定结论立即短路返回。排序采用稳定排序算法，最坏情况下具备 O(n log n) 比较复杂度与 O(n) 辅助存储空间。排序先对带原始索引的快照进行排序以打破平局，随后写回接收者数组中。比较函数返回负数、0 或正数的 int 或 double；NaN 视为相等。在未提供比较函数时，基本类型元素按其字符串表示在 UTF-16 编码单元下的字典序进行排序。包含对象或嵌套数组的数组必须提供显式比较函数。回调通过普通原生脚本调用执行，可以抛出异常，并保留源码栈帧与标准类型化展开。接收者数组与显式实参在遍历开始前从左至右精确求值一次。遍历过程先捕获初始 length，并按序访问每个索引。对后序元素的就地修改对后续回调保持可见。若回调对接收者数组进行了结构化并发修改（如 push/splice 等），即使最终恢复了原长度也强制抛出 Error 异常。在回调中执行 `reserve()` 预留容量是允许的。变更加变 API 详见[动态可增长数组](arrays.md)。Map/Set 查找、变更与有序快照 API 详见 [P1 规范契约](core_language.md#ordered-set-and-map)。

`map` 方法按接收者数组的初始长度一次性分配结果数组，直接存储各回调返回值，并完好保留原始数组。结果数组的元素类型由回调函数返回值推导，包括可空、引用及嵌套数组类型。旧的原地 `transform` 已被废除。Set/Map 暴露只读属性 `.size` 以及方法 `.delete(key)`/`.delete(value)`；数组与字符串保持使用 `.length`。字符串使用 `.includes(text)` 检索子串。

## API 命名规范与设计对齐

当前标准库命名在具有相同语义职责的方法上对齐了 JavaScript 标准习惯：

| 原 API 命名 | 当前标准 API 命名 | 规范契约 |
| --- | --- | --- |
| `Time.monotonicMicros()` | `performance.now()` | 返回 `double` 类型的单调时钟毫秒数 |
| `Time.unixMicros()` | `Date.now()` | 返回 `int` 类型的 Unix 时间戳毫秒数 |
| `Math.pi`, `Math.e` | `Math.PI`, `Math.E` | 只读的 double 数学常量 |
| `Random.nextDouble()` | `Math.random()` | Isolate 局部的带种子随机数，返回 `[0, 1)` 的 double |
| `array.any(predicate)` | `array.some(predicate)` | 遇到首个满足条件的元素即短路返回 true |
| `text.contains(pattern)` | `text.includes(pattern)` | UTF-8 子字符串检索匹配 |
| `set.remove(value)`, `map.remove(key)` | `set.delete(value)`, `map.delete(key)` | 删除条目并返回布尔值表示是否成功删除 |
| `set.length`, `map.length` | `set.size`, `map.size` | 只读属性，当前包含的元素总数 |
| `array.transform(operation)` | `array.map(operation)` | 返回新分配的 `R[]`，从回调返回值推导元素类型 R |
| `Console.write`, `Console.flush` | `print(value)` | 输出文本并自动追加换行符 |
| `Console.readLine()` | 已彻底移除 | 语言不提供内置的控制台输入 API |

`Console` 全局对象已彻底移除。`Random.seed()` 与 `Random.nextInt()` 作为显式的确定性伪随机数扩展 API 保留。旧版 API 名称不设立兼容别名。`map` 方法创建独立的数组存储结构；回调返回的对象引用直接存入新数组中，不进行深拷贝。

Debug 与 Release 构建各自通过了 859 项 C++ 测试与 58 项工具链测试。对 `map` 的测试覆盖了改变元素类型的回调、可空元素、嵌套数组、返回闭包、实参求值顺序、异常栈展开及 GC 回收压力。专门的内存分配测试断言全过程仅分配原始数组、回调闭包以及单次最终结果数组。时钟测试严格断言了毫秒单位及确切的 int/double 原生表示。编辑器 LSP 测试验证了全部新命名与签名；真实的 VS Code 扩展宿主测试同样通关。

## 进程退出状态码规范

`vela run` 将 `main` 函数成功执行的返回值映射为操作系统的进程退出状态码：返回 void 或 null 映射为退出码 0，返回 `[0, 255]` 区间内的 int 整数则直接以此作为进程退出码。其它返回值类型或超出 0..255 范围的整数属于运行时命令错误。返回值不会被控制台自动打印；脚本如需输出必须显式调用 `print`。在宿主 C++ 嵌入场景下，原生执行直接返回强类型计算结果，不触发任何操作系统进程副作用。

命令行参数语法错误返回 64，文件加载/编译/未找到入口错误返回 65，未捕获的运行时异常或非法 main 退出值返回 70。这在全平台上严格遵循标准的 BSD sysexits 退出码规范。脚本本身可以有意返回 `[0, 255]` 范围内的任何代码（包括上述退出码）。`vela check` 语法检查在成功时统一返回 0。
