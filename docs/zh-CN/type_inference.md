# 类型推导

[English](../en/type_inference.md) | [**简体中文**](type_inference.md)

Vela 在两个方向上对表达式进行类型检查：表达式自身合成（synthesize）其类型，外层的变量声明或外围操作则可提供预期类型（expected type）。这属于纯编译期静态推导；不引入 `any` 类型、不引入运行时动态类型探测，也不引入隐式数值转换。命名函数、类方法以及 external 原生声明均严格保留显式的参数与返回值类型标注。

## 泛型调用与统一约束

本文描述当前实现。尚未实现的声明上界、泛型定义检查及上界驱动推导见[泛型系统设计](generics.md)；该设计保留现有值类型、容器不变性和单态化，不采用 Dart 整套类型系统。

泛型函数、类构造和透明泛型构造别名允许省略全部类型实参。编译器从调用实参、外围预期类型和回调签名收集结构化约束，再确定具体特化。显式类型实参仍然有效；类型标注中的 `Box<int>`、`Map<string, int>` 等仍须写完整，不从后续语句反推类型。

```typescript
func identity<T>(value: T): T { return value; }
func apply<T, U>(operation: (T) => U, value: T): U { return operation(value); }
class Box<T> { value: T; }

const id = identity(42);
const box = Box(value: id);
const operation: (int) => int = identity;
const answer = apply(value => value + 2, 40);
const scores: Map<string, int> = Map();
```

普通实参首先提供类型信息；依赖上下文的 lambda、泛型函数值和空构造在所需类型确定后检查。嵌套调用的输出类型变量若依赖后续实参提供的上下文，同样延迟求解。回调书写在提供类型的实参之前不会失去上下文。变量初始化、赋值、字段、构造参数、函数实参、return、条件表达式、`??` 和 `await` 通过同一个表达式检查入口传递预期类型。预期类型是求解线索，不能替代最终赋值关系检查。

数组 `map/reduce`、Set/Map/Completer 构造、Future 工厂和用户泛型共用类型约束求解器。Future 对载荷采用和 `wait` 结果数组的处理属于各自公开 API 的语义；普通回调严格匹配参数数量，数组回调允许末尾参数省略，异常回调按声明的异常类型筛选。这些调用契约不改变共同的推导规则。

推导只合并相同类型及现有的可空关系，不隐式合并 int/double，不创造联合类型或猜测共同业务父类型。已有容器的类型实参保持不变性；数组字面量仍按其数组上下文创建存储。信息不足、约束冲突或最终实参不匹配时产生编译错误，不降级为 dynamic。`const values = Map()` 和无函数类型上下文的 `const operation = identity` 因此仍然报错。只有声明、实参或操作本身确实具有 dynamic 类型时，推导结果才可能包含 dynamic。

每个泛型调用使用独立的推导变量域，求解完成后按规范类型实参请求特化。显式和推断得到的同一组类型实参复用同一个具体声明；后端不持有推导变量。延迟检查保留表达式所在源码位置的流分析事实，并在源码位置应用潜在赋值或调用效果；它不改变运行时实参的从左到右、精确一次求值顺序。

## Lambda 参数推导

当上下文存在预期的函数类型时，Lambda 表达式可按参数位置省略其参数类型标注。该规则统一适用于带类型标注的局部/全局变量、赋值语句、return 语句、类字段与构造函数实参、函数调用实参、回调函数数组、接口方法实现以及完成约束求解的泛型调用。类型别名与可空函数类型同样传递相同的上下文。显式书写参数类型标注依然有效，且必须满足预期的函数签名。

```typescript
func apply(value: int, operation: (int) => int): int {
  return operation(value);
}

func makeAdder(): (int) => (int) => int {
  return left => right => left + right;
}

func main(): void {
  const add: (int, int) => int = (left, right) => left + right;
  print(apply(21, value => value * 2));

  const values = [3, 1, 2];
  values.sort((left, right) => left - right);
  values.forEach(value => { print(value); });
  print(values.reduce((sum, value) => sum + value, 0));
}
```

单个未加类型标注的参数可以省略外层圆括号。多个参数或带有显式类型标注的参数必须保留圆括号。参数个数必须严格匹配。在缺乏预期函数类型上下文时，必须显式标注参数类型：例如 `const twice = (value: int) => value * 2` 是合法的；而 `const twice = value => value * 2` 则会报错指出 `value` 需要显式标注或函数类型上下文。类型检查器不会从函数体内部的运算符或后续调用中猜测参数类型。

## Lambda 返回值推导

显式的返回值类型标注具有最高优先级。未标注时，若外围提供了预期的函数类型，则以此作为目标返回值类型，并对函数体内的每个 return 语句进行校验。该预期类型同时为返回的回调函数及空数组字面量提供推导上下文。

在缺少外围返回值上下文时，类型检查器根据表达式体或代码块内的 return 语句合成返回值类型。相同类型直接合并；`T` 与 `null` 合并为 `T?`，`T` 与 `T?` 同样合并为 `T?`。不同的基本类型（包括 int 与 double）绝不隐式合并。裸 `return;` 与带值的 `return expr;` 不得混用。无 return 语句则推导为 `void`。仅返回 null 的函数必须显式标注可空返回值类型。非 void 的代码块必须在每条分支路径上显式 return 或 throw；推导机制绝不会绕过控制流完整性检查。嵌套 lambda 的返回值各自独立收集，闭包捕获规则保持不变。

## 数组回调上下文

数组高阶方法向同一个类型检查器提供普通函数签名；`sort` 或 `some` 并不享有特殊的专属 lambda 类型推导特权。`map` 提供其输入元素类型，并根据回调函数的实际返回值推导结果数组的元素类型。对于 `reduce`，`initialValue` 提供累加器类型，该类型可与数组元素类型不同。即使回调函数书写在 `initialValue` 之前，直接传递的 lambda 函数体依然在此完整签名下进行类型检查。初始累加值是必填项，省略将导致编译报错。运行时的实参求值顺序依然严格遵循源码书写顺序。

这遵循了 TypeScript 上下文相关的回调类型推导行为。TypeScript 6.0.3 的 `lib.es5.d.ts` 为 `reduce<U>(callbackfn: (previousValue: U, currentValue: T, ...) => U, initialValue: U): U` 声明了独立的累加器类型 `U`。严格模式下，在同一个 `number[]` 上进行数值累加推导出 number/number 参数，而进行字符串累加则推导出 string/number 参数。Vela 采用回调参数在前的传参顺序。数组回调允许省略未使用的末尾参数；编译器会检查其声明的前缀参数，并在调用时精确传入该数量的实参。普通函数调用则保持严格的参数数量契约。更多与 JavaScript 的有意设计差异详见[动态可增长数组](arrays.md)。

## Typed IR 与编辑器契约

语义分析模型持有每个推导完成后的 lambda 签名。Lowering 阶段使用该最终签名来分配参数寄存器、确定返回类型、构建捕获环境以及生成可调用对象布局。源码中可以省略类型标注，但后端绝不会从 AST 标注文本中去二次推导或恢复类型。回归测试验证了等价的显式标注回调与推导回调生成完全一致的指令序列与寄存器分配。

语言服务器（LSP）复用相同的已检查签名，为省略标注的参数提供悬停类型提示（Hover）、成员补全和跳转到定义。未完成的表达式保留已注册签名与已确定的局部类型，诊断不会清空已知的语义事实。代码格式化工具保留省略标注的语法形态，并同时支持带圆括号与单参数无括号形式。

## 实现契约

等价的显式标注与推导回调生成完全相同的字节码、寄存器数量与参数分配。类型推导严格在编译期静态完成，不引入任何运行时动态类型探测、装箱或回调转换开销。
