# 核心语言规范

[English](../en/core_language.md) | [**简体中文**](core_language.md)

本文档定义了 Vela 的核心语言特性、对象模型与运行时契约。函数类型标注仅包含参数类型，后跟 `=>` 及返回值类型；参数名称保留在函数声明与 lambda 表达式中。可运行示例位于 [`examples/`](../../examples/)。`func` 关键字、后置类型标注、强类型约束、显式枚举限定、`T?` 可空类型依然是既定的语言规则。switch 允许空标签共享分支，非空分支不得穿透至后续标签。

显式 `dynamic` 值保留包含 null 在内的实际语言类型，操作和进入具体类型的转换在运行时检查。容器擦除为 dynamic 后仍保留声明的类型实参，`int[]` 不会变成 `dynamic[]`。完整契约见 [JSON 与动态值](json.md)，解析与模型序列化示例见 [`examples/json.vela`](../../examples/json.vela)。

## `never` 与 `null`

`never` 是底类型，表示表达式不可能正常完成并产生值。返回 `never` 的函数必须在每条路径上抛出异常、永久循环或执行另一个不返回的操作；正常 return 或执行到函数末尾会报错。`void` 则允许正常结束，只是不产生返回值。

```vela
func fail(message: string): never { throw Error(message: message); }
func require(value: int?): int {
  if (value == null) fail('missing value');
  return value;
}
```

`never` 表达式可以赋给任意类型，但任何正常产生的值，包括 dynamic，都不能赋给 `never`。分支结果推导不包含无法正常完成的分支，例如 `value ?? fail('missing value')` 得到 value 的非空类型。不返回的调用仍然传播异常，并执行外围适用的 `catch` 和 `finally`。

小写类型 `null` 只包含空值。`never?` 归一化为 `null`，可以赋给可空类型；`null?` 已经可空，因此会报错。`null` 可以出现在类型标注和泛型实参中；没有上下文的 null 初始化器推导为 `null`，集合字面量也适用。推导为 `null` 的变量不能再保存非空值，需要两者时应显式声明可空类型。没有返回值上下文、也没有正常完成路径的 lambda 推导为 `never`。

## 整数字面量

整数字面量默认使用十进制。前缀 `0x` / `0X`、`0b` / `0B`、`0o` / `0O` 分别表示十六进制、二进制和八进制；`0x2a`、`0b101010`、`0o52` 的类型均为 `int`，值均为 42。十六进制数字不区分大小写。单独的前导零不表示八进制，`052` 仍为十进制的 52。

前缀后必须至少有一位符合该进制的数字。不支持数字分隔符、后缀及非十进制浮点数字面量。所有形式统一采用有符号 64 位范围 `[-2^63, 2^63 - 1]`，例如 `-0x8000000000000000` 合法，而正数 `0x8000000000000000` 越界。非十进制写法不会将无符号位模式重新解释为负整数。格式化保留原始前缀和数字拼写，普通整数的字符串输出仍采用十进制。

## 函数、函数值与闭包

脚本函数、方法、lambda 和显式构造函数可以声明默认参数：

```vela
func update(action: (() => void)?): void {
  if (action != null) { action(); }
}
func updateDemo(): void {
  update();
  update((): void => { print('updated'); });
}
```

可空参数未写默认值时，隐式默认值为 `null`：`value: T?` 与 `value: T? = null` 等价，调用时都可以省略。显式默认表达式优先；非空参数未写默认值时仍为必填。此规则覆盖函数、方法、lambda 和显式构造参数，以及可空类型别名。

只有省略实参才会执行默认表达式，显式传入 `null` 不触发默认值。先按源码顺序求值已提供的实参，再按参数声明顺序求值被省略的默认值。默认表达式可以引用前面的参数及其词法环境；实例方法的默认值可以使用 `this`，构造参数的默认值不能访问正在创建的对象。external 可空参数省略时也传入 `null`，但 external 声明不接受其它脚本默认表达式。属性 setter 始终接收赋值操作提供的值。接口实现和 override 必须保留参数的可省略契约；虚方法调用执行实际选中实现的默认表达式。

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

函数值具有固定的位置参数签名。目标函数类型可以省略脚本函数和方法末尾具有默认值的参数，为可选/可变参数内建方法选定合法参数数量，并确定高阶方法的回调与结果类型。没有上下文时，普通固定参数函数值采用完整参数列表；可变参数与回调算法方法值需要函数类型上下文。例如，推断得到的 `values.slice` 接受两个下标，而示例中显式标注的 copy 不接受参数。默认值由底层方法在调用时处理，不在绑定时计算。

类名可以直接作为构造函数值。函数类型上下文决定向普通构造函数提供哪些参数；按字段生成的构造函数把函数值的位置实参映射到对应的构造参数名称，省略参数仍使用普通构造默认值。访问权限和泛型推断遵循直接构造的规则。没有函数类型上下文时，构造函数值采用完整参数列表。

```vela
class Counter { value: int = 42; }
func create(factory: () => Counter): Counter { return factory(); }
func constructorDemo(): void {
  const counter = create(Counter);
  const withValue: (int) => Counter = Counter;
  print(counter.value + withValue(0).value); // 42
}
```

## 泛型函数、类、接口、方法与类型别名

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

类型别名透明，不创建名义类型。泛型调用、构造以及有普通函数上下文的函数/方法值允许整体省略类型实参，按[统一类型推导](type_inference.md)求解；类型标注仍须写完整。函数、class、interface、别名以及实例/静态方法均可声明类型参数，方法参数与所有者参数具有不同身份。

每个参数最多有一个 `extends` 上界。上界可引用其它参数，包括后面的参数，并支持 `T extends Comparable<T>` 这样的递归约束。所有已加载模块中的定义在未使用时也必须正确；无上界 T 不具有任意成员，上界也不提供 `T()` 构造能力。dynamic 和可空值不能证明非空上界。泛型实现与 override 的参数数量、规范化上界和签名必须在参数重命名后等价。完整边界见[泛型系统](generics.md)。

编译器对具体应用进行单态化，保留基本类型的紧凑存储。等价透明别名以及显式/推断得到的同一应用复用特化。独立特化没有固定总数上限；泛型展开和类型嵌套分别限制为 128 层，超出时诊断。依赖按深度优先展开，递归不能通过放宽上界或改成 dynamic 终止。

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

类必须显式声明 `implements`；仅方法名称相符并不足以自动实现接口。方法签名必须完全一致且不发生任何隐式数值转换。将对象作为接口类型使用时，对象的引用身份及其内部数值表示保持不变。普通公开实例方法无论通过类类型还是接口类型调用，都根据接收者的实际类型选择继承链上最具体的实现；静态类型负责成员可见性和调用签名检查。基类方法内部的 `this.method()`、泛型实例方法和显式 getter/setter 遵守相同的动态分派规则。覆盖基类成员必须声明 `override` 并保持签名契约。`super.method()` 显式选择词法父类链上的实现，接收者仍是当前对象。私有方法、静态方法、构造函数和顶层函数不参与实例虚方法分派。`is` 收窄只增加静态类型信息，不改变派发规则。提取的方法值绑定其接收者及提取时选定的实现。

```typescript
class Component {
  build(): int { return 0; }
  render(): int { return this.build(); }
}
class Button extends Component {
  override build(): int { return 42; }
}
func renderComponent(value: Component): int { return value.render(); }
// renderComponent(Button()) 返回 42，基类内部的 this.build() 调用 Button.build()。
```

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

## struct 值类型与内联布局

`struct` 声明具有值复制语义的复合类型。它支持泛型、字段、方法、属性和接口，构造语法及字段默认值规则与 class 相同：

```vela
struct Vec2 { x: double; y: double; }
struct Body {
  position: Vec2;
  velocity: Vec2;
  step(dt: double): void {
    this.position.x += this.velocity.x * dt;
    this.position.y += this.velocity.y * dt;
  }
}
func update(bodies: Body[], dt: double): void {
  for (let i = 0; i < bodies.length; i++) { bodies[i].step(dt); }
}
```

赋值、普通参数传递和返回值复制 struct 的全部字段；嵌套 struct 也复制。引用类型字段复制引用，引用指向的对象仍可共享，因此这不是对象图的深拷贝。`let copy = bodies[0]` 创建独立值，修改 `copy.position.x` 不修改数组元素。普通 `class` 保持引用语义。

struct 字段内联嵌入外层 struct 或 class；`Body[]` 以固定步长连续存储整个值，嵌套值不产生独立对象。`Body?` 具有独立的存在位，`null` 与所有字段为默认值的 Body 不同。包含 string、class、数组、接口或 dynamic 字段的 struct 仍需 GC 扫描其中的引用；纯标量 struct 数组无需扫描元素引用。值类型不保证位于栈上，也不保证所有使用方式都零分配。

对可写位置直接修改字段，或调用同步实例方法、getter、setter，会更新该位置。可写位置包括 `let` 绑定、可写 class 字段和数组元素，也包括这些位置内的可写 struct 字段。接收者和数组下标精确求值一次。每次写入立即可见，回调可以观察已完成的写入，后续异常不会撤销它们；回调改写相邻字段后，后续写入保留这些改动。数组扩容后方法继续访问原数组和下标；元素被删除或可空位置被清空后，再次访问按现有越界或类型转换失败规则报告错误。

`const` struct 绑定及 `const` struct 字段禁止直接字段写入；在这些值上调用方法或 getter 使用独立副本。临时值、普通属性的返回值也按副本调用。`const` 数组或 class 绑定只固定引用，仍允许修改其可写元素或字段。异步 struct 方法接收值副本，挂起期间不借用原位置。

转换到 interface 或 dynamic、提取绑定方法，以及通过宿主或反射传递值时，运行时使用盒保存值快照。对盒调用方法修改盒内的值，不修改装箱前的原值；复制 interface 或 dynamic 引用可以共享同一个盒。转换回具体 struct 后得到值副本。绑定方法在多次调用间保留自己的盒中状态。

同一 struct 类型的 `==` 和 `!=` 逐字段比较，嵌套 struct 递归比较，引用字段遵循其自身类型的相等规则。Map/Set 的 struct 键按字段计算哈希并使用集合相等规则，包括 NaN 与 NaN 匹配、正负零相等；插入与读取遵循值复制规则，不能通过修改取出的值改变已存键。数组迭代绑定也接收元素副本，原地更新应使用下标。

struct 不能继承或被继承，不能声明 abstract 或 override 成员，也不能包含导致无限内联布局的递归字段。通过 class 或数组引用形成的递归关系可以使用。热重载允许修改方法体；已发布 struct 的字段、类型参数和内联布局变更需要 Reload。宿主句柄不暴露编译器内部的聚合体 ABI 或承诺 C++ 二进制布局兼容。

## 按字段生成的类构造模型

类成员默认公开，私有成员的名字以 `#` 开头，声明和访问均保留 `#`，例如 `#count` 与 `this.#count`。不支持 `private` / `public` 修饰符或访问分块。类没有显式构造参数列表时，每个公开实例字段都会生成同名的命名构造参数，字段不需要额外修饰符：

```typescript
class Counter {
  value: int;
  step: int = 1;

  #count: int = 0;

  constructor {
    // 在本代码块执行前，公开参数对应的字段均已完成初始化。
  }

  next(): int {
    this.value = this.value + this.step;
    this.#count = this.#count + 1;
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

自动构造的实参均为命名实参：非空公开实例字段没有默认值时必填，带有默认值时可以省略。可空字段未写初始化器时隐式初始化为 `null`，因此 `text: string?;` 与 `text: string? = null;` 等价，自动构造参数均可省略。此规则也覆盖私有、静态和 `const` 字段；显式初始化器优先。私有字段与静态字段不生成构造参数。非空私有字段需要默认值、字段绑定参数或构造体赋值，非空静态字段必须有初始值。调用语法保留 `ClassName(...)`，不使用 `new`。

隐式 `null` 与显式初始化具有相同的初始化时序和只读约束。可空 `const` 字段省略初始化器后已初始化为 `null`，不能再在构造体中赋值；需要从参数取值时可使用 `constructor(this.field)`。

`constructor { ... }` 是自动生成的构造函数的执行体，并非无参构造函数的声明。在执行该函数体之前，公开参数对应的字段已完成赋值初始化。显式传入的实参按源码书写顺序精确求值一次；被省略的默认值表达式随后按字段声明顺序依次求值，紧接着执行 constructor 代码体。显式提供了实参的字段跳过默认值求值。默认值表达式不可访问 `this` 或调用方的局部变量。每次构造时都会重新执行被省略字段的默认表达式，因此 `values: int[] = []` 会为每个实例创建独立数组。构造过程始终保根新分配的接收者。纯字段构造将实参直接写入字段内存；向显式父类构造转发普通参数时，使用编译期确定的参数窗口。

需要自定义参数的类可以声明 `constructor(name: Type, ...) { ... }`。这份列表就是完整的构造签名，字段不再自动追加参数；`constructor()` 因此表示无参构造。字段默认值仍在函数体前执行，剩余字段由函数体赋值：

```typescript
class Box {
  value: int;
  text: string = '';

  #adjustment: int = 2;

  constructor(initial: int) {
    this.value = initial + this.#adjustment;
  }
}

const box = Box(initial: 40); // value = 42；构造参数中没有 value 或 text
```

参数绑定与字段偏移量在编译期直接解析。运行时不执行任何注解反射，亦不维护参数名称字符串字典。对于显式构造函数，字段默认值在函数体前执行；确定性初始化分析强制要求在每条返回路径上所有字段均已完成初始化。读取未初始化的字段、调用实例方法或在初始化完成前暴露 `this` 均会被静态拦截。

`#name` 仅在声明它的类的词法作用域中可用，闭包保留此作用域。父类和子类可以各自声明同名私有成员，两者的字段槽和方法互相独立；`name` 与 `#name` 也是不同成员。私有成员不参与继承覆盖，不能通过 `super.#name` 访问。静态私有成员必须通过声明类访问。类内部的 `dynamic` 接收者访问使用相同的私有身份，并在运行时检查接收者是否属于声明类；类外无法通过动态分派绕过私有访问。

将 `name` 改为 `#name` 会删除旧成员并新增私有成员。热更新时，仍引用旧成员的版本句柄或闭包会阻止更新；必须先释放这些引用或执行 Reload。保持同一私有名字时，类内闭包按原词法归属访问当前字段。

Vela 提供 `#constructor(...)` 扩展，构造入口只能由本类代码调用，例如由公开静态工厂返回实例。`constructor(this.#value)` 将私有字段绑定到参数名 `value`；调用仍写作 `ClassName(value: ...)`。

实例 `const` 字段只允许在构造期间初始化一次。字段默认值、字段绑定参数和构造体赋值都会计入初始化；分支、循环、异常路径都必须保证每条正常退出路径恰好完成一次初始化。构造结束后禁止赋值、复合赋值和自增自减，动态写入与反射写入同样受限。`const` 是浅层只读绑定，不冻结数组或对象内容，也不要求值是编译期常量。静态 `const` 字段在惰性初始化后只读。

继承构造统一遵循以下四条规则。没有父类时，父类参数列表为空；父类没有参数时也不需要另设规则。

| 子类声明 | 最终构造参数 | 父类调用 |
| --- | --- | --- |
| 不写 `constructor` | 父类构造参数＋本类公开实例字段 | 自动转发父类参数 |
| `constructor { ... }`，没有 `super(...)` | 父类构造参数＋本类公开实例字段 | 自动调用父类，然后执行构造体 |
| `constructor { ... super(...); ... }` | 只有本类公开实例字段 | 按构造体里的显式调用执行 |
| `constructor(...)` | 完整显式参数列表 | 有 `super(...)` 时按代码执行，否则通过 `super.param` 转发 |

前三种形式统一只接受命名实参，即使父类声明了显式参数列表也是如此。显式参数列表支持位置实参与命名实参。
合并签名保留父类参数的名称、类型、必填性和默认值；本类参数按公开实例字段的声明顺序追加。同名参数产生编译错误，需使用显式参数列表消除歧义。
私有字段与静态字段不增加构造参数。私有父类构造不能自动继承或调用。

```typescript
class Component<P> {
  props: P;
}

class ProgressBar extends Component<int> {
  width: int = 100;
  constructor { this.width += 1; }
}

const bar = ProgressBar(props: 42); // props = 42，width = 101
const create: (int, int) => ProgressBar = ProgressBar;
```

显式参数列表支持字段绑定参数 `this.field` 与父类转发参数 `super.param`。它们从目标声明继承类型、必填性和默认值，不能重复标注类型或覆盖默认值。
`this.field` 只能绑定本类实例字段，包括私有字段；`super.param` 必须对应直接父类构造签名中的参数。两种绑定都不引入构造体局部变量；读取字段使用 `this.field`。
普通 `name: Type` 参数仍是局部变量，不能仅凭同名自动赋值或转发。

```typescript
class Counter { value: int = 40; }

class BonusCounter extends Counter {
  bonus: int = 2;
  constructor(super.value, this.bonus);
}

const counter = BonusCounter(); // value = 40，bonus = 2
const other = BonusCounter(41, 1);
```

`constructor(...);` 等价于相同参数列表的空构造体。`super.param` 与显式 `super(...)` 不能混用。
省略显式父类调用时，所有父类必填参数都必须通过转发参数满足；普通参数不会自动转发。
若需要计算父类实参，则使用显式调用：

```typescript
class ComputedCounter extends Counter {
  bonus: int;
  constructor { super(value: this.bonus * 20); }
}

const computed = ComputedCounter(bonus: 2); // value = 40，bonus = 2
```

自动父类调用在本类构造体前执行。显式 `super(...)` 在源码指定的位置执行，每条正常构造路径必须恰好调用一次。公开实例字段必须在进入父类前初始化；私有字段可以延后到 `super(...)` 之后赋值，前提是父类构造期间无法观察到该字段。编译器沿父类构造、虚方法分派及其传递调用分析字段读取、写入、接收者逃逸和闭包捕获；新增子类覆盖也会重新验证祖先的延后初始化条件。所有实例字段在构造正常返回前都必须完成初始化。字段默认值、自动字段参数与 `this.field` 绑定在构造体前生效。下面的私有字段无需假默认值：

```typescript
class Panel { const title: string; const closable: bool = true; }
class TitledPanel extends Panel {
  const prefix: string;
  const #fullHeader: string;
  constructor(this.prefix, rawTitle: string, closable: bool = true) {
    super(title: '${this.prefix}: ${rawTitle}', closable: closable);
    this.#fullHeader = '[${this.title}]';
  }
  get header(): string { return this.#fullHeader; }
}
```

被省略的父类默认值始终在父类的声明环境中求值一次；显式 null 与省略保持区别。
上述规则贯穿泛型替换、多层继承、构造函数值、Typed JSON 和各执行模式。

## 静态成员

```typescript
class Counts {

  static #count: int = 0;

  static next(): int {
    Counts.#count = Counts.#count + 1;
    return Counts.#count;
  }
}
```

非空静态字段必须显式提供初始值；可空静态字段默认初始化为 `null`。每个静态字段在首次被访问时惰性初始化，并在同一次已编译程序的多次执行间持久有效。程序的内存拷贝共享该状态；独立的编译过程拥有独立状态。初始化出现循环依赖将报错 `CyclicInitialization`；初始化失败会重置状态以便后续重新尝试。只要程序保持存活，静态槽位始终作为 GC 根受到保护。静态方法不含 `this`，直接静态解析。

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

const scores: Map<string, int> = {'Vela': 42};
scores.set('Dart', 40);              // void
const score: int? = scores.get("Vela");
const keys: string[] = scores.keys();
const values: int[] = scores.values();

for (const key in scores) {
  const value = scores.get(key);
  if (value != null) { print('${key}: ${value}'); }
}
```

带有静态类型标注的目标变量允许直接简写为 `Set()` 或 `Map()`，类型实参从上下文自动推导。类型保持不变性（invariant）；包含 null 键/值的集合需要可空参数。`get()` 在键缺失时返回 null；`has()` 可清晰区分键不存在还是存储了 null 值。Map 同样暴露 `has`、`delete`、`clear` 和 `size`。快照数组严格保持元素的插入顺序。更新已存在的键保持其原本位置；删除后重新插入则置于末尾。字符串按内容比较，引用类型按对象身份比较，所有 NaN 视作同一个键，+0 与 -0 视作同一个键。普通数值比较保持现有的 IEEE 语义。

键与值采用开放寻址哈希索引及插入顺序链表的强类型连续存储。检索/更新的均摊时间复杂度为 O(1)；生成快照、clear 清空及存活元素追踪为 O(size)。字符串键的哈希计算额外产生 O(key byte length) 开销。扩容与紧凑化偶尔消耗 O(size) 时间。底层分配计入托管堆内存分配压力并由 GC 回收。数组统一使用动态可增长的 `T[]`，不引入冗余的 `List<T>` 容器；详见[动态可增长数组](arrays.md)。

集合循环只绑定一个只读名称。Map 循环按插入顺序遍历键快照；`get(key)` 读取当前值，键已被删除时返回 null。仅需值快照时使用 `for (const value in scores.values())`。循环绑定不支持解构。

### Map 字面量

`{key: value}` 创建普通的 `Map<K, V>`，允许尾逗号及嵌套数组或 Map。键和值都是表达式：变量键 `key` 读取变量，字符串键须写为 `'key'`。有 `Map<K, V>` 上下文时逐项检查；没有上下文时独立推导键和值的类型，不隐式合并 int/double 或异构类型。空 `{}` 需要 Map 上下文；单独的 dynamic 上下文不足以确定键值类型。null 需要已确定的可空或 dynamic 类型，void 不能作为键或值。

```vela
const names = {'language': 'Vela'};
const empty: Map<string, int> = {};
const payload: Map<string, dynamic> = {'name': 'Vela', 'age': 7};
const make: (int) => Map<string, int> = value => ({'answer': value});
```

每次求值创建新的 Map，再按源码顺序执行每个条目的键、键转换、值、值转换及插入，每个表达式只执行一次。重复键覆盖值并保留首次插入位置，所有表达式仍正常求值。失败会停止后续条目，已发生的外部副作用保留。引用值与普通 `set` 一样共享；`const` 不冻结集合。

语句开头的 `{` 和 lambda 的 `=> {` 始终表示代码块，单独的 Map 表达式语句及 lambda 表达式体须加括号。`=> {}` 是空代码块，`() => ({})` 可在 Map 返回类型上下文中返回空 Map。不支持 `<K, V>{...}`、字段名简写、Set 字面量、解构或集合展开。完整执行与工具契约见[Map 字面量设计](destructuring_spread.md)。

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

函数调用（包括递归与闭包）使用原生入口、逻辑调用轨迹和物理栈保护。调用深度或线程栈耗尽返回 `StackOverflow`；普通捕获或返回的错误不阻止原生实例再次调用。可变捕获单元使用共享的 GC 堆对象单元，常量捕获直接保存数值，方法值持有其接收者实例以保持存活。绑定方法和闭包目前会分配 GC 对象。类与接口的虚方法调用共用预先计算的方法选择器表（selector table）。选择器身份保留声明归属及 getter/setter 种类；每个接收者类型将其映射到实际实现。封闭 AOT 只保留可达选择器和接收者所需的实现；证明目标唯一或接收者具体类型后，可以优化为直接调用，而不改变语言的多态行为。

程序元数据在编译或加载时一次性校验，随后以不可变形式共享。类布局表由程序内所有函数共享。所属 Isolate 的生命周期必须长于所有程序、函数句柄及其借用的内存布局。精确根与写屏障完整覆盖闭包捕获、静态槽位及各种集合。大型对象图的扫描尚未按单元素细分时间片预算，因此增量垃圾回收器不承诺硬实时的最大停顿时间。
