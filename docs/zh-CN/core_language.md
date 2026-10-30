# 核心语言规范

[English](../en/core_language.md) | [**简体中文**](core_language.md)

本文档定义了 Vela 的核心语言特性、对象模型与运行时契约。函数类型标注仅包含参数类型，后跟 `=>` 及返回值类型；参数名称保留在函数声明与 lambda 表达式中。可运行示例位于 [`examples/`](../../examples/)。`func` 关键字、后置类型标注、强类型约束、显式枚举限定、`T?` 可空类型依然是既定的语言规则。switch 允许空标签共享分支，非空分支不得穿透至后续标签。

显式 `dynamic` 值保留包含 null 在内的实际语言类型，操作和进入具体类型的转换在运行时检查。容器擦除为 dynamic 后仍保留声明的类型实参，`int[]` 不会变成 `dynamic[]`。完整契约见 [JSON 与动态值](json.md)，解析与模型序列化示例见 [`examples/json.vela`](../../examples/json.vela)。

## 函数、函数值与闭包

```typescript
func add(left: int, right: int): int {
  return left + right;
}

func apply(value: int, operation: (int) => int): int {
  return operation(value);
}

func makeCounter(start: int): () => int {
  let current: int = start;
  return (): int => {
    current = current + 1;
    return current;
  };
}

func functionDemo(): void {
  const operation: (int, int) => int = add;
  print(operation(20, 22));             // 42

  const twice: (int) => int = value => value * 2;
  print(apply(21, twice));              // 42

  const counter: () => int = makeCounter(40);
  print(counter());                     // 41
  print(counter());                     // 42
}
```

函数类型按序仅列出参数类型，不包含参数名称，例如 `(int, int) => int`。通过该类型发起的调用均使用位置实参。函数声明与 lambda 表达式依然必须声明带名称的参数。Lambda 的参数类型可从预期的函数类型中自动推导，例如 `const twice: (int) => int = value => value * 2`。在没有外部参数类型上下文时，显式标注参数的 `(value: int) => value * 2` 会从其函数体中合成推导返回值类型。推导规则与诊断契约详见[类型推导](type_inference.md)。Lambda 使用 `=>` 符号；同时支持表达式体与代码块体。普通函数声明中的参数与返回值类型必须显式标注。闭包捕获遵循词法作用域规则：对捕获的可变 `let` 变量的修改是跨闭包共享的，且其生命周期超越了定义该变量的外层函数调用。捕获 `const` 变量并不使其引用的对象变为不可变。

可以使用类型别名或局部类型推导来避免冗长函数类型的重复书写：

```typescript
type BinaryOperation = (int, int) => int;

func aliasDemo(): void {
  const operation: BinaryOperation = add;
  const inferred = add;
  print(operation(20, 22));             // 42
  print(inferred(20, 22));              // 42
}
```

实例方法值保留接收者，创建绑定时只求值一次。此规则覆盖基本值、字符串、builder、数组与集合内建方法，以及继承方法、接口和 super 绑定。空安全方法访问在接收者为 null 时返回 null，存活的绑定保活接收者。

```vela
func builtinValues(): void {
  const values = [20, 22];
  const copy: () => int[] = values.slice;
  const append: (int) => int = values.push;
  const map: ((int) => string) => string[] = values.map;
  append(42);
  print(copy().length); // 3
  print(map(value => value.toString()).join(',')); // 20,22,42
}
```

函数值具有固定的位置参数签名。目标函数类型为可选/可变参数内建方法选定合法参数数量，并确定高阶方法的回调与结果类型。没有上下文时，普通固定参数内建方法值采用完整参数列表；可变参数与回调算法方法值需要函数类型上下文。例如，推断得到的 `values.slice` 接受两个下标，而示例中显式标注的 copy 不接受参数。默认值由底层方法在调用时处理，不在绑定时计算。不引入动态分派或第二套可调用 ABI。

## 泛型函数、类与类型别名

```typescript
type UserId = int;
type IntOperation = (int) => int;

func identity<T>(value: T): T {
  return value;
}

class Box<T> {
  value: T;

  get(): T {
    return this.value;
  }
}

func genericDemo(): void {
  const id: UserId = identity<int>(42);
  const box: Box<int> = Box<int>(value: id);
  print(box.get());                     // 42
}
```

类型别名是透明的名字替换，并非全新的名义类型。泛型类型实参必须显式指定。编译器对静态泛型使用进行具体特化展开（monomorphization），从而保留紧凑的原生基本类型存储，而非将每个 `T` 泛型值装箱为堆对象。等价的透明别名会复用相同的特化版本。泛型函数和类模板在实例化时进行完整类型检查。独立特化没有固定的总数上限；泛型依赖展开深度与类型嵌套深度分别限制为 128 层，超出时产生诊断。依赖采用深度优先展开，使分叉递归在形成指数规模的工作队列前触达深度边界。

## 接口与显式实现

```typescript
interface Measurable {
  measure(): int;
}

class Rectangle implements Measurable {
  width: int;
  height: int;

  measure(): int {
    return this.width * this.height;
  }
}

func area(value: Measurable): int {
  return value.measure();
}

func interfaceDemo(): void {
  const rectangle: Rectangle = Rectangle(width: 6, height: 7);
  print(area(rectangle));               // 42

  const measure: () => int = rectangle.measure;
  print(measure());                     // 42
}
```

类必须显式声明 `implements`；仅方法名称相符并不足以自动实现接口。方法签名必须完全一致且不发生任何隐式数值转换。将对象作为接口类型使用时，对象的引用身份及其内部数值表示保持不变。对具体类的方法调用保持静态解析；通过接口发起的调用使用运行时动态分派。提取的方法值会自动绑定其接收者实例。

## 普通枚举与穷尽性 Match 表达式

```typescript
enum Brightness {
  light,
  dark,
}

func brightnessName(value: Brightness): string {
  return match (value) {
    Brightness.light => "light",
    Brightness.dark => "dark",
  };
}

func nullableValue(value: int?): int {
  return match (value) {
    null => 0,
    let present => present,
  };
}

func enumDemo(): void {
  print(brightnessName(Brightness.light)); // light
}
```

Match 表达式对其选择器求值一次，并选取唯一匹配分支，不存在任何 fallthrough 穿透。所有可能的值必须被完整覆盖；遗漏枚举分支属于编译错误。`let` 用于绑定被选中的值；`_` 为通配符。除允许常规的可空类型拓宽外，所有分支必须产生相同类型。选择器覆盖所有非空分支后跟一个 null 分支（或反之），支持对可空类型进行类型细化（narrowing）。上例中的通用匹配分支 `let present` 在前面的 null 分支之后被求值，因此其绑定的类型自动排除了 null。模式匹配不发生隐式转换；字符串格式化遵循[字符串语境规则](strings.md)。传统的 `switch` 语句允许空标签共享分支，拒绝非空分支中可达的穿透路径。常量模式支持整数、布尔值、字符串以及显式限定的枚举成员。不支持浮点数字面量模式、解构模式或模式 guard；通配符与变量绑定分支可用于处理其它选择器类型。

## 类型测试与受查类型转换

```typescript
func rectangleArea(value: Measurable): int {
  if (value is Rectangle) {
    return value.width * value.height;
  }
  return value.measure();
}

func requireRectangle(value: Measurable): Rectangle {
  return value as Rectangle;
}
```

`is` 运算符测试运行时对象类型，并在匹配的分支路径上对稳定的变量绑定进行类型细化。`as` 运算符强制检查对象类型并在类型不匹配时报告运行时类型转换失败。二者均不执行任何数值类型转换。数值转换始终为显式的 `toInt()`/`toDouble()` 操作。

## 按字段生成的类构造模型

类成员默认公开，仅私有成员需要标记 `private`。类没有显式构造参数列表时，每个公开实例字段都会生成同名的命名构造参数，字段不需要额外修饰符：

```typescript
class Counter {
  value: int;
  step: int = 1;

  private count: int = 0;

  constructor {
    // 在本代码块执行前，所有必需字段均已完成初始化。
  }

  next(): int {
    this.value = this.value + this.step;
    this.count = this.count + 1;
    return this.value;
  }
}

func main(): void {
  functionDemo();
  genericDemo();
  interfaceDemo();
  enumDemo();

  const counter: Counter = Counter(value: 40);
  print(counter.next());               // 41
}
```

自动构造的实参均为命名实参：公开实例字段没有默认值时必填，带有默认值时可以省略。可空类型仅表示允许传入 null，不表示可以省略：`text: string?` 仍为必填，`text: string? = null` 才可省略。私有字段与静态字段不生成构造参数。私有字段需要默认值或由显式构造函数赋值，静态字段必须有初始值。调用语法保留 `ClassName(...)`，不使用 `new`。

`constructor { ... }` 是自动生成的构造函数的执行体，并非无参构造函数的声明。在执行该函数体之前，所有必需字段已完成赋值初始化。显式传入的实参按源码书写顺序精确求值一次；被省略的默认值表达式随后按字段声明顺序依次求值，紧接着执行 constructor 代码体。显式提供了实参的字段跳过默认值求值。默认值表达式不可访问 `this` 或调用方的局部变量。每次构造时都会重新执行被省略字段的默认表达式，因此 `values: int[] = []` 会为每个实例创建独立数组。生成的构造函数将新分配的接收者实例始终保持保根，并将实参直接写入字段内存；绝不将所有字段实参暂存在独立寄存器中直至函数体启动。

需要自定义参数的类可以声明 `constructor(name: Type, ...) { ... }`。这份列表就是完整的构造签名，字段不再自动追加参数；`constructor()` 因此表示无参构造。字段默认值仍在函数体前执行，剩余字段由函数体赋值：

```typescript
class Box {
  value: int;
  text: string = '';
  private adjustment: int = 2;

  constructor(initial: int) {
    this.value = initial + this.adjustment;
  }
}

const box = Box(initial: 40); // value = 42；构造参数中没有 value 或 text
```

参数绑定与字段偏移量在编译期直接解析。运行时不执行任何注解反射，亦不维护参数名称字符串字典。对于显式构造函数，字段默认值在函数体前执行；确定性初始化分析强制要求在每条返回路径上所有字段均已完成初始化。读取未初始化的字段、调用实例方法或在初始化完成前暴露 `this` 均会被静态拦截。`private` 仅对声明该成员的类本身可见，静态访问与动态分派均遵循此限制。

继承类的构造函数不会自动接收父类的构造参数。父类有必填参数时，子类必须在构造体中显式调用 `super(...)`：

```typescript
class Counter {
  value: int;
}

class BonusCounter extends Counter {
  bonus: int;

  constructor {
    super(value: this.bonus * 20);
  }
}

const counter = BonusCounter(bonus: 2); // value = 40, bonus = 2
```

如果父类参数都有默认值，或者父类没有必填参数，子类可以省略 `super()`；父类构造函数会在子类构造体前隐式执行。省略 `super(...)` 而父类仍有必填参数时，编译器报告错误。

## 静态成员

```typescript
class Counts {
  private static count: int = 0;
  static next(): int {
    Counts.count = Counts.count + 1;
    return Counts.count;
  }
}
```

静态字段强制要求提供初始值。每个静态字段在首次被访问时惰性初始化，并在同一次已编译程序的多次执行间持久有效。程序的内存拷贝共享该状态；独立的编译过程拥有独立状态。初始化出现循环依赖将报错 `CyclicInitialization`；初始化失败会重置状态以便后续重新尝试。只要程序保持存活，静态槽位始终作为 GC 根受到保护。静态方法不含 `this`，直接静态解析。

## 显式数值转换

```typescript
const whole: int = 42.9.toInt();
const fraction: double = whole.toDouble();
```

`int` 和 `double` 暴露 `toInt()` 与 `toDouble()` 方法。Double 转 int 向零截断；非有限数值（NaN/Infinity）以及超出 `[-2^63, 2^63)` 范围的数值抛出 `InvalidNumericConversion` 异常。Int 转 double 遵循 IEEE binary64 舍入规则，超过 53 位有效位时可能损失精度。同类型转换保持原值不变（包括带符号零）。这些转换操作不分配任何 GC 对象。算术运算、赋值、函数参数及返回值依然严禁隐式数值转换。`is` 和 `as` 运算符不执行数值转换。

`int.parse` / `int.tryParse` 与 `double.parse` / `double.tryParse` 将完整字符串显式解析为数值。整数解析检查有符号 int64 边界，tryParse 对非法文本返回 null。进制选择、浮点语法和错误见[字符串数值解析](numeric_parse.md)。

## 固定字节存储

`ByteArray(length: int)` 拥有零初始化、固定长度的原始字节。下标读取返回 int，写入要求 0..255 的 int。`ByteArray.from(int[])` 和 `slice()` 复制存储；`ByteData.view(bytes, ...)` 共享并保活所有者，通过 Endian 枚举提供受检查、允许非对齐的整数与浮点访问。两种字节类型都不隐式转换为 int[] 或 string。详见[字节存储与带类型视图](byte_data.md)。

## 正则表达式

`RegExp` 与 `RegExpMatch` 是按引用身份比较的内建引用类型，支持可空标注、字段、闭包、不变容器与异步载荷。RegExpMatch 由搜索创建，全部属性和匹配下标只读；坐标按 Unicode 码点计数。详见[正则表达式](regexp.md)与[示例](../../examples/regexp.vela)。

## 有序 Set 与 Map

```typescript
const names: Set<string> = Set<string>();
names.add("Vela");                  // bool: 插入成功返回 true
names.has("Vela");                  // bool
names.delete("Vela");               // bool: 删除成功返回 true
names.clear();                       // void
const count: int = names.size;
const items: string[] = names.values();

const scores: Map<string, int> = Map<string, int>();
scores.set("Vela", 42);              // void
const score: int? = scores.get("Vela");
const keys: string[] = scores.keys();
const values: int[] = scores.values();
```

带有静态类型标注的目标变量允许直接简写为 `Set()` 或 `Map()`，类型实参从上下文自动推导。类型保持不变性（invariant）；包含 null 键/值的集合需要可空参数。`get()` 在键缺失时返回 null；`has()` 可清晰区分键不存在还是存储了 null 值。Map 同样暴露 `has`、`delete`、`clear` 和 `size`。快照数组严格保持元素的插入顺序。更新已存在的键保持其原本位置；删除后重新插入则置于末尾。字符串按内容比较，引用类型按对象身份比较，所有 NaN 视作同一个键，+0 与 -0 视作同一个键。普通数值比较保持现有的 IEEE 语义。

键与值采用开放寻址哈希索引及插入顺序链表的强类型连续存储。检索/更新的均摊时间复杂度为 O(1)；生成快照、clear 清空及存活元素追踪为 O(size)。字符串键的哈希计算额外产生 O(key byte length) 开销。扩容与紧凑化偶尔消耗 O(size) 时间。底层分配计入托管堆内存分配压力并由 GC 回收。数组统一使用动态可增长的 `T[]`，不引入冗余的 `List<T>` 容器；详见[动态可增长数组](arrays.md)。

## Unicode 码点与字素簇

```typescript
const text: string = "e\u0301🇨🇳";
const points: int[] = text.codePoints();
const clusters: string[] = text.graphemes();
const count: int = text.graphemeLength;
const first: string = text.graphemeSlice(0, 1);
```

字符串始终为经过校验的合法 UTF-8 序列。现有的 `length`、索引访问、`slice` 切片及搜索偏移量统一按 Unicode 码点计算。字素簇 API 使用扩展字素簇（Extended Grapheme Cluster），切片范围为左闭右开。非法的切片范围抛出 `IndexOutOfBounds`。不执行任何 Unicode 规范化归一化或针对特定语言环境的微调。字符分段采用版本固定的 utf8proc 2.12.0 / Unicode 18.0.0。`codePoints()` 与 `graphemes()` 通过线性单次遍历生成确切大小的数组，绝不在索引元素时重复全量扫描字符串。字素簇长度统计与切片采用线性扫描，不提供常数时间 O(1) 的随机访问保证。

## 执行机制与所有权模型

函数调用（包括递归调用与闭包调用）使用显式的 VM 栈帧，而非递归调用 C++ 解释器函数。固定的栈帧深度上限抛出 `StackOverflow`；执行失败后虚拟机依然可安全发起下一次调用。可变捕获单元使用共享的 GC 堆对象单元，常量捕获直接保存数值，方法值持有其接收者实例以保持存活。绑定方法和闭包目前会分配 GC 对象。接口调用采用预先计算的方法选择器表（selector table）；具体类的方法调用保持直接调用。选择器仅为被实际引用的接口方法按源码顺序分配。构建类元数据时仅遍历各个类显式声明的实现列表，而非为每个类遍历所有接口。

程序元数据在编译或加载时一次性校验，随后以不可变形式共享。类布局表由程序内所有函数共享。所属 Isolate 的生命周期必须长于所有程序、函数句柄及其借用的内存布局。精确根与写屏障完整覆盖闭包捕获、静态槽位及各种集合。大型对象图的扫描尚未按单元素细分时间片预算，因此增量垃圾回收器不承诺硬实时的最大停顿时间。
