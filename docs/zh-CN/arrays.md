# 强类型动态数组

[English](../en/arrays.md) | [**简体中文**](arrays.md)

`T[]` 是紧凑、可变且支持动态增长的强类型数组，元素类型保持不变性（invariant）。显式使用 `dynamic[]` 可以保存混合类型，各元素保留实际语言类型，进入具体类型槽位时仍进行检查，详见 [JSON 与动态值](json.md)。系统不提供独立的 `List<T>` 类型，不进行隐式数值转换，不支持稀疏索引赋值，也不允许直接修改 `length` 属性。

```vela
const values: int[] = [];
values.reserve(1000);
const alias = values;
for (let index = 0; index < 1000; index = index + 1) {
  values.push(index);
}
values.unshift(42);
const first: int? = alias.shift();
const removed: int[] = values.splice(0, 2, 40, 42);
const last: int? = values.pop();
values.clear();
```

`const` 仅阻止对变量重新赋值绑定，并不阻止数组内部状态的修改。所有引用别名均能实时观察到数组长度与元素内容的变动，包括跨越动态扩容。`slice()` 与 `splice()` 返回的被删除元素数组拥有独立的底层连续存储，但继续共享内部所引用的对象实例。可运行示例见 [`examples/dynamic_arrays.vela`](../../examples/dynamic_arrays.vela)。

## API 方法与错误处理

| 成员 | 返回类型 | 行为说明与时间复杂度 |
| --- | --- | --- |
| `length` | `int` | 只读属性，当前存活元素数量；O(1) |
| `capacity` | `int` | 只读属性，当前已分配的元素容量（至少等于 length）；O(1) |
| `push(...items: T[])` | `int` | 在末尾追加零个或多个元素；返回新长度；追加 k 个元素均摊复杂度为 O(k) |
| `pop()` | `T?` | 弹出并返回最后一个元素，数组为空时返回 null；O(1) |
| `unshift(...items: T[])` | `int` | 在开头按实参顺序插入元素；返回新长度；O(n + k) |
| `shift()` | `T?` | 移除并返回第一个元素，数组为空时返回 null；O(n) |
| `splice(start?: int, deleteCount?: int, ...items: T[])` | `T[]` | 替换指定区间元素并返回包含被删除元素的新数组；O(n + k) |
| `slice(start?: int, end?: int)` | `T[]` | 独立的浅拷贝数组，默认截取整个数组；O(n) |
| `fill(value: T, start?: int, end?: int)` | `T[]` | 用指定值填充指定区间并返回接收者数组自身；O(n) |
| `reverse()` | `T[]` | 原地反转数组元素并返回接收者数组自身；O(n) |
| `join(separator?: string)` | `string` | 分隔符默认为逗号；null 元素贡献空文本；返回合并后的字符串 |
| `clear()` | `void` | 在 O(1) 内将 length 置为零；保留已分配的底层 capacity 供后续复用 |
| `reserve(capacity: int)` | `void` | 确保数组至少具备指定的非负容量且不改变当前 length；发生扩容时复杂度为 O(n) |

`T?` 进行了自动展平：在 `int?[]` 上调用 `pop()` 返回 `int?`，而非 `int??`。空数组弹出与弹出原本存储的 null 元素均返回 null。`splice()` 返回的数组长度可清晰区分是没有元素被移除，还是移除了存储的 null。

`...items` 表示位置变长实参：`push(1, 2, 3)` 传入三个值；这并非语言通用的 Rest 参数声明或展开运算符语法。所有显式传入的实参在执行数组操作前从左至右精确求值一次。原有的命名固定参数依然支持；变长项必须作为位置实参提供。

`slice` 和 `fill` 的区间默认均为 `[0, length)`。负数端点从末尾倒数计算；端点数值自动限制（clamp）在 `[0, length]` 区间内。若 end 小于 start 则选取空区间。`splice` 以相同方式规范化 start 参数，并将 deleteCount 限制在剩余元素长度范围内；负数 deleteCount 视为 0。`splice(start)` 会删除从 start 到末尾的所有元素，而 `splice()` 不删除任何元素。`splice(start, 0, ...items)` 仅执行纯插入操作。

非法索引访问与传入负数的 reserve 将抛出 `Error` 异常；内存分配或容量耗尽返回运行时可恢复的 OutOfMemory 状态。当前底层表示将 length/capacity 限制为 `uint32_t`，并在实际分配前校验超大请求。类型不匹配在编译期诊断报错；失败的操作使数组的内容、长度及版本号完好保持未修改状态。现有的受查索引访问始终以 length 为边界，即使已分配 capacity 更大亦严格受限。

在元素移除或执行 clear 时底层容量不会自动缩容。确切的空闲容量属于底层实现细节。尝试 reserve 比当前容量更小的值属于无操作（no-op）。Reserve 不会创建未初始化的空洞或已初始化的占位元素。底层缓冲区在数组被垃圾回收或其所属 Isolate 销毁时得到释放。

固定原始字节存储使用 [ByteArray 与 ByteData](byte_data.md)。ByteArray 是每元素一个字节、长度固定且显式复制的独立引用类型，与 int[] 不共享表示或隐式转换；ByteData 视图共享 ByteArray 存储并通过 GC 保活所有者。

## 内存表示、垃圾回收与性能

GC 对象头与数组对象自身的引用身份永不移动。紧随对象头之后的轻量描述符持有数据指针、32 位容量及 64 位结构化版本计数器。初始元素数量不超过 4 个的数组直接在同一次对象分配中利用 4 个内联槽位；更大的数组则分配外部连续缓冲区。动态增长会重新安置外部缓冲区，同时完整保留所有指向数组对象的外部引用。其它对象头结构保持不变。

Int/enum 元素占 8 字节，double 占 8 字节，bool 占 1 个原生 bool 字节，引用类型占 1 个原生指针宽度。可空引用元素保持相同指针宽度；可空基本类型与 `dynamic` 元素使用带标签的 `Value` 槽位。容量以元素个数计算，与单元素物理宽度无关。

Push 与插入操作在需要时按几何级数扩容，并附带溢出受查的算术计算。扩容时仅复制实际存活的前缀元素。显式的 reserve 直接一次性申请目标容量。批量插入仅扩容一次，并使用 `memmove` 将后续元素一次性平移；绝不为每个被移动的基本类型元素进行装箱操作。容量充足时，单元素 push 走无任何内存分配的直接快速路径。变长实参在求值后保存在强类型数组中进行保根；被删除的元素在变更提交前完成复制。默认初始化严格限制在新建数组的存活元素范围内。Clear 无需清空整块底层分配内存，因为 GC 绝不扫描超出 length 的空闲槽位。Pop 与移除操作会将空出的末尾槽位显式清零。

固定对象分配与外部连续缓冲区均计入 `allocated_bytes()` 与 `committed_bytes()`。重新分配会准确核算新旧缓冲区的更替；内联空间始终属于固定对象的一部分。小对象内存块保留常规的 GC 空闲链表策略。这些计数器不是进程工作集（RSS）或物理内存峰值度量。

扩容属于 GC 内存分配边界。在垃圾回收可能运行前，数组对象及其待插入的引用会被提前保根；新引用的写入通过写屏障发布。标记阶段仅扫描 `[0, length)` 范围内的强类型引用切片。已被移除的元素与废弃的内联载荷不会维持外部对象的存活。清除阶段与 Isolate 销毁严格保证只释放一次持有的外部缓冲区。

宿主 C++ 提供的 `IntegerElements()` / `StringElements()` 接口借用当前活跃缓冲区。数组必须保持被保根，且这些借用切片绝不能跨越数组扩容或结构化修改。原生运行时算法持有被保根的数组值并在回调后重新获取元素指针，而非跨回调持有脆弱的底层裸指针。

## 回调函数与并发结构修改检测

`map`、`forEach`、`reduce`、`some`、`every`、`findIndex` 和 `sort` 在触发回调前会先捕获接收者数组及其结构修改计数器。在进行下一次数组访问或短路返回前，每次回调返回后均会重新核验该计数器。Push、非空的 pop/shift/clear 以及非空的 splice 操作均会递增该计数器。成对的 push/pop 即使最终恢复了原始长度也能被准确检测出来。

若在回调期间发生并发结构修改，将抛出状态为 `ConcurrentModification` 的 `Error` 异常，并通过标准原生栈展开进行传播。已发生的修改本身保持生效，不进行事务回滚。回调函数可以安全修改已有元素的值、修改其它独立数组或预留容量。遍历过程中对后续元素的就地修改保持可见。异常抛出不会使数组陷入死锁状态，原生实例依然可继续正常复用。

回调函数接收 `(value, index, receiver)` 参数，对于 reduce 则接收 `(accumulator, value, index, receiver)`，允许省略未使用的末尾参数。`forEach` 忽略回调函数的返回值。`reduce(callbackFn, initialValue)` 强制要求提供初始累加值，从索引 0 开始遍历；当数组为空时直接返回初始值而不触发回调。

`sort(compareFn?)` 原地排序并返回接收者数组自身，且属于稳定排序（stable sort）。比较函数可返回 int 或 double；NaN 视为相等。默认比较器按 UTF-16 编码单元字典序对基本类型的字符串表示进行排序。排序使用被保根的快照和原始索引打破平局，随后写回排好序的元素。排序具有 O(n log n) 的比较复杂度和 O(n) 的辅助存储。比较函数对接收者既有元素的修改不会改变已建立的快照；但在比较器中执行结构化修改依然会抛出 Error。

检查所需的接收者/版本号参数窗口一次性准备并在循环各次迭代中复用。回调不会在每次迭代中分配参数窗口，也不为该检查创建多余的 GC 对象。静态类型检查、编辑器补全、悬停提示及命名参数补全均消费完全一致的数组方法签名。

## 与 JavaScript 的有意设计差异

- `T[]` 始终保持强类型不变性、紧凑连续存储及严格类型约束，具有受查索引访问与只读的 length。不支持 undefined、不产生稀疏空洞，亦不进行隐式类型强转。
- 在空数组上调用 pop/shift 返回 null。fill 要求传入强类型的 T 值；谓词方法必须返回 bool。
- reduce 强制要求提供 initialValue（即使在非空数组上亦然）。回调函数保持作为第一个参数；初始值确定累加器的静态类型。
- 在高阶方法回调中进行结构化修改会抛出 Error（包括最终恢复了原长度的 push/pop 对）。这避免了产生与 `T[]` 契约不兼容的稀疏结果或空洞丢失。闭包机制取代了 JavaScript 中动态绑定的 `thisArg`。
- join 与默认排序支持基本类型与可空元素。对象与嵌套数组排序必须提供显式比较函数；join 不调用隐式的对象转换。数字输出采用与 JS 相同的 -0、NaN、Infinity 及十进制/指数记法，而 int 保留 Vela 完整的 64 位精确表示。
- capacity、reserve 与 clear 是显式提供的性能扩展 API。已删除的旧式 copy/insert/removeAt 方法不设立兼容别名，统一迁移至 slice()/splice()。

本次重构统一了现有 API 并新增了 shift/unshift/splice。未引入 filter、find、includes、concat、flat 或产生拷贝的 toSorted/toReversed 方法。
