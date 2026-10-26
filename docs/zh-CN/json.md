# JSON 与动态值

[English](../en/json.md) | [**简体中文**](json.md)

本文定义 JSON API 与受检查动态值的契约。可运行示例见 [`examples/json.vela`](../../examples/json.vela)。

Vela 采用 Dart 的 JSON 值模型：解析得到原生基本值、数组和有序 Map，序列化直接接受这些值。公开名称为 `Json.parse` 和 `Json.stringify`，不提供独立的 JSON 节点类或节点构造 API。

```vela
func main(): void {
  const payload: Map<string, dynamic> = Map<string, dynamic>();
  payload.set('name', 'Vela');
  const items: dynamic[] = [1, true, null];
  payload.set('items', items);

  const text: string = Json.stringify(payload);
  const root: dynamic = Json.parse(text);
  const name: string = root.get('name') ?? '';
  print(name);
}
```

本规格覆盖内存字符串、JSON 所需的动态类型，以及通过公开 `toJson()` 方法转换对象。不加入文件、流或网络 I/O、通用 Map 字面量、reviver、replacer、自定义转换回调或格式化选项。[字节存储与视图](byte_data.md)具有独立的类型与所有权契约。

## 公开 API 与值模型

| API | 契约 |
| --- | --- |
| `Json.parse(source: string): dynamic` | 解析一个完整 JSON 值，返回原生 Vela 值 |
| `Json.stringify(value: dynamic): string` | 将值图编码为紧凑 JSON，拒绝不支持的值与循环引用 |

`Json` 是无需 import 的标准静态类，不可构造。其声明与现有标准对象一样，参与标准源码依赖闭包与编译缓存指纹计算。

| JSON 值 | 解析后的 Vela 值 |
| --- | --- |
| `null` | Vela `null` |
| 布尔值 | `bool` |
| 不含小数点或指数的整数词法单元 | 有符号 64 位 `int` |
| 含小数点或指数的数字词法单元 | `double` |
| 字符串 | 不可变 UTF-8 `string` |
| 数组 | `dynamic[]` |
| 对象 | `Map<string, dynamic>` |

接受所有 JSON 根值。解析出的数组始终为 `dynamic[]`，即使当前元素全部为整数。对象数组的元素是 Map，不会自动构造业务类实例。Map 的 `get` 对缺失字段和存储的 JSON null 均返回 null；需要区分时使用 `has`。对象访问使用已有 Map 方法，不增加通过属性名访问字段的简写。

## 动态类型与受检查操作

`dynamic` 可以保存任意 Vela 值，包括 null、基本值、枚举、实例、函数和容器。它是语言类型，不是 JSON 专用包装，也不能替代前端的无效类型。`void` 不是可存储的值。`dynamic` 已包含 null，额外的 `?` 属于冗余标注并被拒绝。

赋值到 `dynamic` 保留原值、引用身份和语言类型。从 `dynamic` 进入具体类型时插入运行时类型检查；初始化、赋值、实参、返回值、字段、下标写入及跨入带类型槽位的闭包捕获遵循相同规则。检查成功不执行数值转换或容器转换，失败走现有受检查转换的错误路径。

```vela
const values: dynamic[] = Json.parse('[7, "Vela", true]');
const id: int = values[0];       // 运行时检查成功。
const name: string = values[1];  // 运行时检查成功。
const wrong: int = values[1];    // 抛出可捕获的类型转换错误。
```

允许对动态接收者访问成员、调用方法、调用函数值、使用下标和运算符，并在运行时检查。分派以接收者真实类型及其已有公开 API 为准，在执行前检查成员存在性、可写性、实参名称与数量、参数类型和元素约束。不存在的成员或非法操作产生可捕获的类型错误。动态分派不能访问私有成员、增加实例字段或绕过只读属性。从对象取得的方法值必须保留接收者。

动态操作沿用 Vela 普通语义：整数溢出仍受检查，int/double 转换仍须显式执行，数组访问仍检查边界，条件必须得到 bool。空安全运算符沿用既有求值顺序与短路规则。不引入 truthiness、JavaScript 隐式转换或任意用户自定义运算符。

`is` 和 `as` 检查保留的语言类型。现有流分析扩展到 dynamic 局部变量和参数；重新赋值或可能修改捕获绑定的调用会使收窄失效。字段和下标元素不进行类型提升。动态调用缺少已知函数签名时，不能为未标注的 lambda 参数提供上下文，继续使用普通推断诊断。泛型可调用值必须先完成特化再擦除；动态调用不接受显式类型实参。

动态类型仅由显式标注、声明为 dynamic 的结果或动态操作引入。`dynamic[]` 上下文允许混合数组字面量；数组字面量处于 `dynamic` 上下文时构造 `dynamic[]`。缺少这类上下文时，数组与返回类型继续使用现有严格推断规则。未解析的名字或推断失败不得降级为 dynamic。

## 容器与业务模型转换

Vela 容器保持不变性。将 `int[]` 存入动态变量不会把存储变成 `dynamic[]`；通过动态调用向该数组插入字符串，必须在修改前失败。Map 的键值和 Set 的元素写入同样必须保留声明的类型约束。

解析得到的 `dynamic[]` 也不会因为当前元素都是整数就变成 `int[]`。通过显式检查并复制完成转换：

```vela
const values: dynamic[] = Json.parse('[1, 2, 3]');
const integers: int[] = values.map(value => value as int);
```

这里的 map 立即遍历：返回一个新的带类型数组，或传播首次元素检查失败，不向调用方交付部分填充的结果。对象数组同样需要显式业务转换函数，检查必填字段、可空性与字段类型。把解析出的 Map 转换为 `User` 不会创建 User 实例。第一版不包含按目标类型自动解码或生成业务模型转换代码。

动态数组元素和动态 Map/Set 槽位使用带标记存储。各值保留已有相等与哈希规则所需的信息，包括枚举类型身份。静态基本类型容器继续采用紧凑存储。普通集合支持动态键；JSON 编码另行要求每个实际对象键都是字符串。

动态 `map` 从回调保留的返回值签名确定结果数组的元素类型。动态 `reduce` 保留初始累加值的实际类型；初值为 null 时，回调的累加器必须声明为可空类型或 dynamic。即使数组为空，也会检查回调契约。

## 解析契约

解析接受完整 JSON 值语法，包括混合数组。根值前后只允许 JSON 空白。空输入、额外根值、尾随垃圾、注释、尾随逗号、开头 BOM、非法转义、未转义控制字符和非法 Unicode 均报错。UTF-16 转义代理对解码为合法 UTF-8，拒绝孤立代理项。内嵌 NUL 保留为字符串内容，不能截断键或值。

整数词法单元必须位于 `[-2^63, 2^63 - 1]`，超出范围即拒绝，包括底层 JSON 库原本可能改用 uint64 或 double 表示的值。小数和指数形式直接转换为 binary64，不先经过整数转换。有限结果按 IEEE 754 舍入，包括下溢到带符号零；溢出为无穷大时报错。NaN 与 Infinity 字面量不属于 JSON。整数 `-0` 成为零，浮点负零保留。

对象键为字符串。重复键后者覆盖前者，同时保留该键第一次插入的位置，沿用现有有序 Map 契约。键按解码后的字符串内容比较。

完整输入及所有数字和字符串均验证成功后才返回结果。返回的字符串与容器由 Isolate 堆拥有，不借用输入字符串或临时 JSON arena。

## 编码与业务对象

直接编码支持 null、bool、int、有限 double、string、数组，以及实际键全部为字符串的 Map。带具体类型的数组和 Map 直接遍历，不先把整份存储转换成动态容器。元素与值递归检查。枚举不能按其整数存储进行编码。函数、Set、Expected、Future、可变 String 和字节存储对象不属于直接可编码值。

业务类实例通过公开、非泛型、无参数的实例方法 `toJson` 编码；其结果再次按相同规则处理，该方法必须返回一个值。不自动反射字段、枚举 getter，或按类的内存布局转换对象。

```vela
class User {
  public param name: string;
  public param age: int;

  public toJson(): Map<string, dynamic> {
    const result: Map<string, dynamic> = Map<string, dynamic>();
    result.set('name', this.name);
    result.set('age', this.age);
    return result;
  }
}

func main(): void {
  const users: User[] = [User(name: 'Vela', age: 7)];
  print(Json.stringify(users));
}
```

对象在值图中每出现一次，就为该次出现调用一次 `toJson`。方法抛出的异常经普通 VM 栈帧原样传播并保留原始调用栈；返回不支持的值产生编码错误。编码同步执行，不支持异步转换返回的 Future。

循环检测跟踪当前遍历路径上的引用身份，包括正在执行 `toJson` 转换的对象。`toJson` 返回自身、容器引用自身和间接环均失败；不同分支共享同一引用是合法的，分别序列化。

遍历沿用数组顺序和 Map 插入顺序。调用用户代码前，编码器记录活动容器的结构修改版本；调用返回后，使用保存的遍历位置前检查版本。发生结构修改时走现有并发修改错误路径，即使长度最终恢复也报错。替换已有条目是允许的；尚未访问的条目将读取替换后的值。编码不提供快照，也不回滚用户副作用。

输出不包含无意义空白或末尾换行。字符串转义引号、反斜线和控制字符，其余合法 UTF-8 保留。整数精确输出十进制；有限 double 使用不受 locale 影响的最短往返表示，并保留小数点或指数，使再次解析后仍为 double。浮点负零输出为 `-0.0`。

## 错误、所有权与资源使用

| 失败 | 结果 |
| --- | --- |
| JSON 语法、Unicode 或数字范围错误 | `Error`，code 为 `JsonDecodeError`，消息含从零开始的 UTF-8 字节偏移 |
| 不支持的值或键、非有限数字、循环引用 | `Error`，code 为 `JsonEncodeError`，消息指出失败值的路径 |
| 动态转换、调用、成员或下标错误 | 现有受检查转换、类型或越界错误路径 |
| 转换期间发生结构修改 | 现有 `ConcurrentModification` 路径 |
| `toJson` 抛出异常 | 原始异常及其调用栈 |
| 分配或存储资源耗尽 | 现有可恢复 `OutOfMemory` 路径 |

意外 EOF 的偏移为输入字节长度；其它解码偏移指向原始输入，不能使用解转义后的字符串位置或修改后的解析缓冲区位置。JSON 专用错误码必须完整穿过 builtin/VM，不能被压成通用原生调用失败。内存耗尽时，错误对象分配沿用现有 VM 失败契约。

解析器在分配安全点前保根部分构造的容器和值。编码器在调用和分配期间保根输入、活动容器、转换接收者和转换结果。动态字段、数组、Map、捕获、全局变量和挂起局部变量按真实值标记追踪；引用发布走现有写屏障。成功或失败都必须释放 JSON 临时资源，外部分配计入运行时内存核算。

遍历采用显式状态，避免无界 C++ 递归。复杂度为输入或输出大小的线性成本，加上有序 Map 操作的期望成本；处理重复键不能扫描已有全部键。共享图可能产生重复输出，因此编码成本按输出大小与转换工作量衡量。不设置语言级固定嵌套深度，资源耗尽显式返回错误。

## 实现范围与完成条件

现有[标准声明](../../compiler/frontend/standard_environment.cc)、[builtin 签名](../../compiler/frontend/builtins.h)、[运行时值](../../runtime/value.h)与 [JSON 基础设施](../../base/json.h)界定集成边界。现有 JSON helper 拥有 yyjson arena，运行时值不能直接暴露其中的节点。复用已经固定版本的 yyjson 依赖与共享底层设施；保留原始数字词法单元和源偏移，避免底层库的数字转换破坏 int64 契约。runtime 不依赖 compiler/tooling。

| 阶段 | 必须完成的工作 |
| --- | --- |
| 标准环境 | Json 及依赖声明只注册一次；保留名称预留、局部遮蔽、源码 overlay 和缓存指纹契约 |
| 前端 | 增加 Dynamic，接通具体类型边界检查、混合字面量上下文、动态操作与收窄失效；保留容器不变性 |
| 运行时类型身份 | 类型擦除后仍区分枚举与 int、完整函数签名、类身份、容器类型实参和 Future 实参；复用规范类型元数据 |
| 运行时存储与 GC | 数组和集合增加真正的动态 tagged 槽位，追踪所有包含动态值的存储；保留基本类型快路径 |
| IR 与编译器 | 显式表示受检查转换与动态分派；标记分配、调用、读写和可抛出效果；保留求值顺序 |
| VM 与错误 | 按公开成员和准确签名分派；接通解析、编码及普通 `toJson` 调用之间的编码续执行状态，不从 C++ builtin 递归进入 VM |
| 字节码镜像与缓存 | 序列化必要类型及分派元数据，加载时验证索引与布局，更新格式及构建身份，拒绝不兼容镜像 |
| 工具链 | 补全、悬停、诊断、格式化、文档、调试器和持久化 REPL 共享受检查签名与动态类型信息 |
| 交付 | 增加 `examples/json.vela`；全部生产路径与必要验证完成后才勾选功能清单 |

[运行时布局生产者](../../compiler/runtime_layouts.cc)保留类、接口与枚举的名义身份、函数与 Expected 的结构签名，以及 Future/Completer 的载荷签名。数组和集合保留完整元素约束。枚举在擦除槽位中装箱以保留身份，强类型枚举数组仍使用整数存储。元数据由 Isolate 持有，不依赖编译结果的生命周期；加载字节码镜像时重新构造并验证。

必要验证覆盖全部 JSON 根类型、混合与对象数组、缺失和 null、重复键顺序、Unicode 转义与内嵌 NUL、int64 端点和越界、binary64 边界与负零、非法输入偏移、非字符串键、不支持类型、循环与共享分支、toJson 成功/失败/修改容器，以及捕获错误后的 VM 复用。类型测试覆盖所有 dynamic 到具体类型的边界、私有与只读成员、命名调用、函数签名、枚举身份、泛型实参、不变容器写入、字面量上下文和推断失败。GC 压力测试覆盖部分解析结果、转换调用、动态槽位、新 Isolate 中加载的镜像和 REPL 保留值。

实现完成边界执行相关 Debug/Release 回归、工具测试、仓库静态检查及 Release benchmark。测量解析与编码吞吐、分配及保留内存、动态分派、具体类型边界检查和普通强类型代码的回退幅度。完成运行后的聚合测量保存在[滚动基准摘要](../../benchmarks/results/rolling_summary.json)。

## 参考

- [Dart jsonDecode](https://api.dart.dev/dart-convert/jsonDecode.html) 与 [jsonEncode](https://api.dart.dev/dart-convert/jsonEncode.html)。
- [Dart JSON 实现与转换契约](https://github.com/dart-lang/sdk/blob/main/sdk/lib/convert/json.dart)。
- [Vela 核心类型与集合](core_language.md)、[异常](exceptions.md)与[编译缓存](compilation_cache.md)。
