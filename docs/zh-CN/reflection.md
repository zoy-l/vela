# 运行时反射设计

> 状态：设计草案，待实现。本文定义第一阶段的反射 API 与执行契约；显式登记入口尚未定稿，见第 13 节。本文中的 API 均为拟新增接口。

Vela 的反射提供类型与成员描述、字段和属性读写、方法调用、对象构造及枚举查询。
脚本与 C++ 宿主消费同一份编译产物元数据，JIT 与 AOT 使用相同的类型和执行契约。
成员是否公开与是否登记为可反射成员分别决定；公开成员不会自动进入反射目录。

第一阶段只实现反射。特性系统、装饰器、编辑器控件、序列化、存档身份迁移、顶层函数与
全局变量反射、运行时开放泛型特化及动态代码生成不属于本文的完成范围。
反射不增加类型声明、成员声明或调用的专用语法，也不为上述消费者预置业务规则。

下面的例子假定 `Counter` 类型、`value` 字段和 `add` 方法已通过显式登记输入选中。
登记输入的源码或配置写法不在示例中虚构。

```vela
import 'sdk:reflect' as reflect;

class Counter {
  value: int = 10;

  add(amount: int): int {
    this.value += amount;
    return this.value;
  }
}

func main(): void {
  const counter = Counter();
  const type = reflect.typeInfo<Counter>();
  const field = type.findField('value');
  if (field != null) {
    field.setValue(counter, 20);
  }

  const method = type.findMethod('add');
  if (method != null) {
    const arguments = reflect.CallArguments(items: [
      reflect.CallArgument(value: 5, name: 'amount'),
    ]);
    const result = method.invoke(counter, arguments);
    print(result as int); // 25
  }
}
```

## 1. 设计依据与现有基础

公共描述模型主要参考 [.NET System.Reflection](https://learn.microsoft.com/en-us/dotnet/api/system.type?view=net-10.0)，
查询范围参考 [Java Core Reflection](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/Class.html)。
采用类型描述与字段、属性、方法、构造、参数描述分离的模型，以及发现成员与执行成员访问分离的边界。
Vela 不复制这些平台的程序集装载、访问提权、重载绑定、数值自动转换和运行时代码生成能力。

| 参考接口 | Vela 的采用方式 |
| --- | --- |
| .NET `Type`、`MemberInfo` | `TypeInfo` 与公共成员描述；适配现有类型结构和声明身份 |
| .NET `FieldInfo`、`PropertyInfo` | 保留存储字段与 getter/setter 属性的区别 |
| .NET `MethodInfo`、`ConstructorInfo`、`ParameterInfo` | 描述正常调用契约；参数绑定和执行继续复用语言实现 |
| Java `getDeclaredFields` / `getFields` | 用统一查询对象明确区分本类声明和包含继承的范围 |
| .NET Native AOT 的限制 | 只操作已编译的具体类型和入口，保留反射可达闭包 |

现有实现提供以下基础，但尚未提供本文的公共反射接口：

| 现有实现 | 可复用能力与必须补齐的边界 |
| --- | --- |
| [声明身份](../../compiler/frontend/modules/declaration_ids.h) | 会话内稳定声明、具体泛型身份及删除后的身份退休 |
| [布局描述](../../base/layout_description.h)与[原生映像](../../runtime/native/native_image.h) | 结构类型、声明、字段、签名、构造及枚举事实；需要补齐反射选择和查询关系 |
| [动态成员登记](../../compiler/ir/lowerer_dynamic.cc)与[成员表](../../runtime/isolate/member_table.h) | 现有动态执行的公开实例成员；不是反射授权目录，也未覆盖全部反射种类 |
| [动态成员执行](../../runtime/native/native_dynamic.cc) | 字段读写、访问器调用、绑定方法及类型检查 |
| [动态参数绑定](../../runtime/dynamic/dynamic_call.cc) | 有序实参、具名绑定、rest、类型检查及枚举转换 |
| [C++ Type](../../include/vela/type.h)与[对象操作](../../include/vela/objects.h) | 已知逻辑类型的宿主读写、构造和枚举成员读取 |
| [版本分派](../../runtime/native/native_dispatch.h) | 当前发布选择、稳定字段访问、迁移与代码版本所有权 |

## 2. 显式登记与类型结构

前端必须接收一份显式登记集合，并将其中的类型、成员、访问器、构造和枚举成员绑定到
规范声明身份。登记不存在的声明、错误的成员种类或无效的具体类型实参属于编译错误。
类型登记不自动登记它的字段、方法、属性或构造；签名引用某个类型也不自动登记该类型的成员。
继承查询只返回实际登记的成员，不因查询子类而扩大基类登记集合。

登记集合是编译输入，不由运行时执行某段注册代码后临时推断。AOT 的可达性分析、JIT 的
源码检查和开发会话的候选准备必须消费同一份绑定结果。登记的用户入口及其输入格式是
M0 的待决事项；不能以内部测试手工构造的登记记录代替生产入口完成验收。

类型检查必需的结构信息与反射成员目录分开。`typeInfo<T>()` 和 `typeOf(value)` 可以描述
未登记类型的身份、种类、可空性和类型实参，但不会因此开放成员查询或操作。
`TypeInfo.isRegistered` 表示该具体名义类型是否已登记。
对未登记名义类型进行成员目录查询报告 `ReflectionUnavailable`，不能把它伪装成零成员类型。

第一阶段允许登记 private 成员的描述信息，但反射操作仅开放已登记的公开访问路径。
登记 private 元数据不授予读取、写入、调用或构造权限，即使反射调用发生在声明类内部也不例外。
进一步的非公开访问能力需要独立设计，不通过隐藏开关或修改查询过滤器提供。
反射登记不修改模块的静态导出集合和名称绑定规则。

普通数值、字符串、null、void、dynamic、数组、集合、函数与异步标准类型均可获得结构描述。
它们没有对应种类的成员时，相关查询合法返回空集合或 null。标准类型的成员开放仍需要
显式登记；不能从已有动态内建成员表自动推导。捕获单元、绑定方法包装类和异步恢复存储等
编译器内部布局不作为可发现的用户类型。

## 3. 模块入口与类型发现

脚本通过普通 SDK 模块 `sdk:reflect` 使用接口。SDK 的加载、别名和名称隔离沿用
[模块契约](modules.md)，不新增关键字或隐式全局名称。
下表中的默认参数表示拟议 SDK 契约，由标准声明与绑定共同实现。

| API | 返回类型 | 行为 |
| --- | --- | --- |
| `typeInfo<T>()` | `TypeInfo` | 获取静态指定类型的规范结构描述，不要求构造 T，也不登记其成员 |
| `typeOf(value: dynamic)` | `TypeInfo` | 获取实际值的类型；数组和集合保留声明的元素类型，不按当前内容猜测 |
| `getTypes(module: string)` | `TypeInfo[]` | 枚举当前程序中该模块登记的具体名义类型 |
| `findType(module: string, name: string, typeArguments: TypeInfo[] = [])` | `TypeInfo?` | 按声明名及完整类型实参查找已登记类型，不在运行时生成新特化 |
| `revision()` | `int` | 获取当前反射上下文的发布版本；用于识别查询结果所属发布 |

`module` 使用程序中已有的规范模块 URI。接口只查询当前程序图，不读取文件、解析导入、
执行模块初始化或加载新代码。未知模块的 `getTypes` 返回空数组，`findType` 返回 null。
导入别名、源码行号和内部特化显示名不构成查找身份。
`name` 是模块内未限定声明名；泛型实参通过独立类型描述传入，不在运行时解析类型文本。

透明别名按展开后的真实类型描述，不独立列入类型目录。泛型目录只列出已登记、已编译的
具体应用。对同一类型的多次登记合并为同一身份；相同模块和声明的不同具体类型实参保持区别。
类型枚举按声明名及规范类型实参键排序，不依赖哈希表顺序，也不将枚举位置作为持久身份。

`typeOf(null)` 返回 Null 类型，`typeInfo<void>()` 可用于描述无返回值的签名；void 仍不是
可存储的语言值。泛型函数可以转发 `typeInfo<T>()`，具体特化负责产生类型描述，抽象参数不进入运行时。

## 4. TypeInfo 与类型身份

`TypeInfo` 由运行时创建，没有公开构造函数，公共属性只读。它是绑定反射上下文的类型句柄，
不是供用户修改的类型定义，也不保存可由外部解引用的内存地址。

| 属性或方法 | 类型 | 含义 |
| --- | --- | --- |
| `id` | `TypeId` | 所属 Isolate 内的规范类型身份，支持相等比较和哈希 |
| `name` | `string` | 未限定类型显示名；不用于判断身份 |
| `module` | `string?` | 名义声明的规范模块 URI；无所属声明的结构类型为 null |
| `kind` | `TypeKind` | 类型种类，覆盖现有语言的基础、名义、容器、函数及异步类型，并包含 Void、Null、Dynamic |
| `isNullable` | `bool` | 是否是显式可空类型；dynamic 的 null 接受规则单独由其类型种类决定 |
| `isRegistered` | `bool` | 当前发布是否将此具体名义类型登记到反射目录 |
| `typeArguments` | `TypeInfo[]` | 具体泛型、数组、集合和异步类型的实参，按类型声明顺序排列 |
| `baseType` | `TypeInfo?` | 声明的直接父类，不把可空性重新传播为父类声明的一部分 |
| `interfaces` | `TypeInfo[]` | 语义模型确认的接口集合，使用确定性顺序 |
| `signature` | `SignatureInfo?` | 函数类型的结构签名，其它类型为 null |
| `isInstance(value: dynamic)` | `bool` | 使用现有运行时类型约束检查该值 |
| `isAssignableFrom(other: TypeInfo)` | `bool` | 判断 other 类型的值能否赋给本类型，不执行转换 |

`TypeKind` 的取值为 Void、Null、Dynamic、Bool、Int、Double、String、MutableString、
Array、Set、Map、Function、Class、Interface、Enum、Future、Completer、Timer、
CancellationToken、ByteArray、ByteData、RegExp 和 RegExpMatch。
普通脚本类、标准名义类和 `external class` 均属于 Class；宿主存储不成为新的语言基础类型。

`isAssignableFrom` 的方向是目标接收来源。类型关系使用与 `is`、`as` 和赋值边界相同的
规则，保留容器不变性、枚举名义身份、函数签名、可空性及 Future 载荷。
它不因 int 的当前数值可被 double 表示而返回 true，也不把结构相似的类当成同一名义类型。

可空名义类型的 `baseType`、`interfaces` 和类型实参描述其原声明关系，赋值与实例判断另外处理
可空性；成员目录与对应非可空声明共用登记。`SignatureInfo` 提供
`parameters: SignatureParameterInfo[]` 和 `returnType: TypeInfo`；每个结构参数包含
`type`、`isOptional` 和 `isRest`，不伪造声明参数名和默认表达式信息。
Void 只参与签名和身份描述，`isInstance` 对任何值均返回 false；任一方为 Void 的
`isAssignableFrom` 报告 `TypeError`，不把签名结果当作可赋值的值类型。

`TypeId` 不是原生映像局部编号、`ClassLayout::id()`、C++ 指针或可跨运行保存的整数。
结构类型的身份包含完整嵌套结构；名义类型包含具体泛型身份。跨 Isolate 的描述不能混用。
`TypeId` 与 `MemberId` 只用于比较和哈希，不提供从原始数值恢复句柄的接口，也不能在
旧身份仍可观察时复用为另一个目标。它们需要的身份元数据同样纳入所有权与追踪契约。

## 5. 成员描述与查询

字段、属性、方法、构造及枚举成员描述实现公共 `MemberInfo` 协议。公共信息为
`id: MemberId`、`name: string`、`declaringType: TypeInfo`、`kind: MemberKind`、
`visibility: MemberVisibility` 和 `isStatic: bool`。
`MemberKind` 区分 Field、Property、Method、Getter、Setter、Constructor 和 EnumMember。
`MemberId` 包含所属反射上下文和稳定声明身份，不向用户暴露布局槽号或函数表索引。
枚举成员身份可由稳定枚举身份与序号确定；枚举结构变更仍要求完整 Reload。

| TypeInfo API | 返回类型 |
| --- | --- |
| `getFields(query: MemberQuery = MemberQuery())` | `FieldInfo[]` |
| `findField(name: string, query: MemberQuery = MemberQuery())` | `FieldInfo?` |
| `getProperties(query: MemberQuery = MemberQuery())` | `PropertyInfo[]` |
| `findProperty(name: string, query: MemberQuery = MemberQuery())` | `PropertyInfo?` |
| `getMethods(query: MemberQuery = MemberQuery())` | `MethodInfo[]` |
| `findMethod(name: string, query: MemberQuery = MemberQuery(), typeArguments: TypeInfo[] = [])` | `MethodInfo?` |
| `getConstructor()` | `ConstructorInfo?` |
| `getEnumMembers()` | `EnumMemberInfo[]` |
| `findEnumMember(name: string)` | `EnumMemberInfo?` |

`MemberQuery` 是普通标准库参数对象，字段为：

| 字段 | 可选值 | 默认值 |
| --- | --- | --- |
| `scope: MemberScope` | `Declared`、`IncludeInherited` | `IncludeInherited` |
| `storage: MemberStorageFilter` | `Instance`、`Static`、`All` | `All` |
| `visibility: MemberVisibilityFilter` | `Public`、`Private`、`All` | `Public` |

这些值由标准枚举表达，不引入标志运算语法。过滤条件只收窄已登记目录，不改变访问权限。
成员本身的 `MemberVisibility` 只包含 Public 和 Private，不使用查询中的 All 值。
查找区分大小写。不符合查询范围、未登记或不存在的成员返回 null；有效空目录返回空数组。
无构造能力的类型，包括接口、枚举及未登记构造的类，`getConstructor()` 返回 null。
在非枚举类型上进行枚举成员查询报告 `TypeError`。

继承解析先使用语义模型确定的有效声明，再应用登记和查询过滤，不能让未登记的派生覆写
悄悄退回基类实现。返回描述的 `declaringType` 是真实声明所有者。
调用方可以明确查询基类的声明，但在派生对象上调用该描述仍遵守现有具体类调用语义，
不额外引入虚方法分派。接口方法描述使用接口选择器，按实际接收者进行正常接口分派。

普通方法查找使用声明名。泛型方法只查找已有具体特化，类型实参通过独立列表匹配；
缺少所需实参返回 null，不把某个碰巧已编译的特化作为默认目标。
本设计不为普通用户方法引入重载。

成员枚举按名称和规范具体类型实参键排序；枚举成员按原声明序号排序。
返回数组是独立快照，用户修改数组不改变登记目录。列表元素仍是第 9 节定义的当前版本句柄，
不因一次枚举而把后续执行固定到旧代码。

### 5.1 字段与属性

| 描述 API | 类型或返回类型 | 行为 |
| --- | --- | --- |
| `FieldInfo.type` | `TypeInfo` | 字段的完整声明类型 |
| `FieldInfo.getValue(receiver: dynamic)` | `dynamic` | 读取字段，保留原值和名义类型 |
| `FieldInfo.setValue(receiver: dynamic, value: dynamic)` | `void` | 完整类型检查后写入，使用正常写屏障 |
| `PropertyInfo.type` | `TypeInfo` | 访问器的值类型 |
| `PropertyInfo.canRead` / `canWrite` | `bool` | 当前登记与公开访问路径是否允许读或写 |
| `PropertyInfo.getter` / `setter` | `MethodInfo?` | 已登记的相应访问器描述，缺少时为 null |
| `PropertyInfo.getValue(receiver: dynamic)` | `dynamic` | 调用正常 getter |
| `PropertyInfo.setValue(receiver: dynamic, value: dynamic)` | `void` | 检查 setter 参数后调用正常 setter |

实例成员要求合法、非 null 的接收者。静态成员统一传入 null；传入其它值报告 `TypeError`，
不忽略错误接收者。成员描述只绑定声明，不在内部永久保存某个对象。
调用方跨分配保存接收者时使用已有脚本引用或 C++ 根句柄。

属性按逻辑名字聚合 getter/setter，两个访问器保留独立的登记、可见性与调用身份。
属性只要存在已登记公开访问器就符合 Public 查询；纯 private 属性符合 Private 查询。
`getter`、`setter` 可描述已登记的非公开访问器，但相应调用和 `canRead` / `canWrite` 不获得提权。
同一访问器不重复列入普通方法枚举。

元数据查询不会读取字段值、执行 getter、初始化静态槽或求值默认表达式。
实际 `getValue` 则包含正常字段访问的初始化语义，包括热重载新增字段的首次初始化；
不能直接读未初始化存储。静态字段访问通过正常全局槽服务，不套用实例字段偏移。

### 5.2 方法、构造与参数

| 描述 API | 类型或返回类型 | 行为 |
| --- | --- | --- |
| `MethodInfo.parameters` | `ParameterInfo[]` | 脚本可见的有序参数，不包含隐藏接收者和 ABI 参数 |
| `MethodInfo.returnType` | `TypeInfo` | 声明返回类型，无返回值时为 Void |
| `MethodInfo.typeArguments` | `TypeInfo[]` | 已编译方法具体特化的类型实参 |
| `MethodInfo.isAsync` | `bool` | 声明是否为 async；不根据返回 Future 推测此标志 |
| `MethodInfo.invoke(receiver: dynamic, arguments: CallArguments)` | `dynamic` | 执行受检查的当前声明调用 |
| `ConstructorInfo.parameters` | `ParameterInfo[]` | 正常构造签名，包含生成构造的可省略字段参数 |
| `ConstructorInfo.invoke(arguments: CallArguments)` | `dynamic` | 执行正常构造，返回实际实例 |

Vela 每个类只有一个构造入口，因此没有构造重载列表。未登记构造不会因为取得了类型描述
就自动开放。构造必须完成普通初始化检查，不提供未初始化对象分配或绕过构造的字段填充入口。
显式构造和生成字段构造分别遵守[核心语言构造契约](core_language.md#按字段生成的类构造模型)。

`ParameterInfo` 是只读元数据记录，包含 `name`、从零开始的 `position`、`type`、
`isOptional`、`isRest` 和 `hasDefault`。参数不是独立可调用声明，不暴露可跨签名变化复用的参数 ID。
`hasDefault` 只表示声明中存在默认表达式，不提供 `defaultValue`，也不提前求值。
类型签名中的参数位置和声明参数信息分开；不能为函数类型补出源码中不存在的参数名。

返回 void 的反射调用在动态值边界返回 null；它不使 void 成为可存储类型。
async 或普通返回 Future 的方法返回原 Future，不隐式 await、不额外调度或取消任务。
用户异常、源码调用栈、调用深度、执行预算和宿主调用约束与普通执行一致。

`EnumMemberInfo.ordinal: int` 是原枚举序号，`value: dynamic` 是保留该枚举名义身份的值。
它不等于裸 int，也不因仅登记了部分枚举成员而重编号。

## 6. CallArguments 与调用顺序

`CallArgument` 与 `CallArguments` 使用普通标准库类和现有构造语法。其可构造数据形状为：

| 类型 | 字段 |
| --- | --- |
| `CallArgument` | `value: dynamic`；`name: string? = null` |
| `CallArguments` | `items: CallArgument[] = []` |

`name == null` 表示位置参数，非空字符串表示具名参数；空字符串不是合法具名参数。
有序记录保留位置与具名顺序以及重复名称，避免先放入 Map 后丢失重复参数错误。
参数包中的表达式按普通构造和数组规则求值，进入 `invoke` 前已完成求值，不由反射重新执行。
调用开始后在所属线程读取一份稳定的有序实参快照，再进行绑定和用户代码调用。

方法调用沿用现有参数名称、位置、可选和 rest 契约。出现具名参数后不能再传位置参数；
重复绑定、未知名称、必需参数缺失和错误 rest 绑定均拒绝。构造实参保持现有具名参数规则，
不为反射放开位置构造参数。未来顶层函数值反射不能借此改变现有函数值调用的参数规则。

一次成员操作按以下顺序执行：

1. 从所属上下文选择一个当前发布，解析稳定目标身份，并验证当前登记与公开访问权限。
2. 检查成员种类、接收者和实例/静态调用形式。
3. 对调用绑定全部参数，检查重复、缺少、名称、rest 与完整类型约束；字段写入检查目标值类型。
4. 建立必要的根、枚举表示和临时参数存储，进入正常字段、访问器、方法或构造执行路径。
5. 以原始语言类型返回结果或传播原失败，退出时释放本次临时根和借用。

不执行 int/double 自动转换、容器复制转换或枚举猜测转换。省略与显式 null 独立表达，
默认表达式只由正常构造路径在确实省略时执行。
无效接收者或实参不会进入目标方法、setter、构造体或用户默认表达式。
运行用户代码之后的副作用沿用普通调用语义，不承诺回滚。

接收者和参数包从进入 API 时起保根；上述校验阶段可能分配错误或临时存储，不能等到
即将执行目标时才建立根。可重载生成构造使用既有稳定字段身份与参数存在性契约，
反射在调用时适配当前构造签名，不把旧参数序号当成当前字段身份，也不以 null 表示省略。

## 7. 错误与权限边界

脚本继续抛出现有 `Error` 体系，C++ 继续返回 `Result`。不新增反射异常包裹层，
不把用户抛出的错误统一改写为反射失败。下表的两个反射专用 code 是拟新增的明确分类，
其余复用[现有执行状态](../../base/execution_status.h)。

| 结果或 code | 条件 |
| --- | --- |
| null / 空数组 | 有效查找没有匹配项；有效已登记目录为空 |
| `ReflectionUnavailable` | 未登记名义类型的成员目录查询，或仍存在的声明已退出当前反射登记 |
| `ReflectionAccessDenied` | 已取得描述，但尝试执行非公开访问路径 |
| `TypeError` | 错误成员种类、静态调用接收者错误、属性缺少相应访问器、非法 Void 赋值判断或参数绑定形式错误 |
| `InvalidCast` | 实例接收者或实参、写入值不满足完整类型约束 |
| `TargetDeleted` | 已取得的成员身份在当前发布中已删除 |
| `InvalidIsolate` | C++ 边界混用不同 Isolate 的上下文、描述或值 |
| `ExecutionStopped` | 所属运行环境已停止，不能继续解析或执行 |
| 原资源或用户错误 | 分配失败、深度限制、初始化循环、超时、getter/setter/构造/宿主抛错等 |

编译时登记错误保留模块、声明或配置位置和具体原因。运行时错误保留稳定 code 与必要的
声明、版本信息；消息文本不作为程序判别协议。目标删除的判断优先于当前登记检查，
避免旧句柄被同名新增声明重新绑定。

`canRead` / `canWrite` 方便消费者展示可用操作，但不替代操作时对当前发布的检查。
两次操作之间可能发生合法 Hot Reload；执行边界必须处理实际变化。
访问器存在但未登记时执行报告 `ReflectionUnavailable`；已登记 private 访问器报告
`ReflectionAccessDenied`；根本没有相应访问器才报告 `TypeError`。

## 8. 脚本与 C++ 的共同运行时

C++ 新增 `vela::Reflection` 上下文及非模板类型、成员描述接口。此处是拟议接口映射，
不是对现有头文件的可用性声明。

| C++ 接口 | 契约 |
| --- | --- |
| `Reflection::Bind(NativeInstance&)` | 绑定封闭实例，保留必要程序所有者 |
| `Reflection::Bind(NativeDispatch&)` | 绑定开发会话，操作时选择当前发布 |
| `Reflection::TypeOf(scope, value)` | 接收同一 Isolate 的 `Local<Value>`，返回 `Result<TypeInfo>` |
| `Reflection::FindType` / `GetTypes` | 与脚本发现接口相同；缺失通过 optional 或空集合表达 |
| `Type<T>::Describe()` | 将现有已绑定逻辑类型桥接到非模板类型描述，不重新创建名义身份 |
| 描述查询 | 返回拥有明确元数据根的描述或独立结果，不返回跨安全点借用的原生地址 |
| 字段和属性读写 | 使用 `HandleScope` 与 `Local<Value>`；读取返回 `Result<Local<Value>>`，写入返回 `Result<void>` |
| 方法和构造调用 | 使用同一有序实参契约；结果或异常载荷在返回时已保根 |

从可重载 `NativeInstance` 绑定时必须采用其所属开发会话，不能借此将句柄固定到旧发布。
C++ 实参表示可以借用同一作用域内的名称和根句柄，其借用只覆盖调用期间，不能保留在
描述中。类型化便利接口继续通过现有 codec 编解码，不新增另一套宽松的值转换规则。
C++ 描述的复制、释放与操作遵守 Isolate 所属线程约束；跨线程工具仅接收复制的展示数据，
实际访问调度回所属线程。

SDK 与 C++ 层都调用共享 runtime 的反射解析、权限检查及执行服务。现有 `Type<T>`、
动态访问、普通静态调用和 JSON 构造仍有各自真实调用方；反射复用其权威语义，
不删除仍在使用的路径，也不为反射复制一套对象构造和访问器执行实现。

`external class` 只描述已绑定并显式登记的公开构造、方法和访问器。
C++ 数据字段不成为脚本字段，不暴露宿主指针、偏移或私有布局，不参加脚本字段迁移。
宿主 binding 在开发会话内固定的限制不变。

## 9. Hot Reload、所有权与查询一致性

反射沿用[热重载契约](hot_reload.md)中稳定声明、不可变发布和完整 Reload 的边界，
不扩大可接纳的字段类型、函数签名、父类和枚举结构变更范围。

类型与成员句柄保存稳定身份和所属上下文。可重载句柄不捕获历史函数版本、字段槽号、
内存块地址或历史 NativeImage。后续查询与操作解析当前发布；删除成员、撤回登记或
改变权限后返回对应错误。类型身份本身不变，登记状态不参与类型相等判断。

元数据、登记目录、分派入口和对象布局作为同一候选在安全点发布。
一次 API 调用在解析时借用并保活同一快照，不暴露半发布结果。
批量查询返回的成员身份集合属于该次调用选择的版本；元素句柄的后续属性读取和操作仍选择
各自调用时的当前发布。`ParameterInfo` 等复制记录只描述取得时的签名，不授权执行旧签名。
消费者需要组合多次纯元数据查询时，可以比较前后的 `revision()` 并在变化时重新取得结果；
不能对已经执行的 getter、构造或方法套用自动重试。

| 变更 | 反射行为 |
| --- | --- |
| 修改方法或访问器函数体 | 句柄下次调用当前实现，已执行帧保留原版本 |
| 重排字段 | 句柄按声明身份解析；枚举仍使用本文的确定性顺序 |
| 新增带默认值的字段 | 接纳且显式登记后出现在新目录；实际首次读取复用正常初始化 |
| 修改已有默认表达式 | 更新 `hasDefault` 等描述；已有对象值不重置 |
| 删除成员 | 先遵循既有存留代码依赖检查；接纳后旧反射句柄报告 `TargetDeleted` |
| 删除后同名新增 | 新身份；旧反射句柄不复活、不按名字重绑 |
| 撤回登记 | 新查找不再返回该项；旧句柄报告 `ReflectionUnavailable` |
| 修改既有 ABI 或枚举结构 | 沿用 `RequiresReload`，不为反射生成适配层 |

单纯持有描述不构成可执行旧代码依赖，不能因打开了元数据查询工具就永久阻止字段删除。
真正取得并保存的函数值继续遵守已有的版本保留契约；第一阶段不新增隐式捕获版本的反射操作。

封闭上下文保活其程序和必要元数据；会话句柄保留稳定类型根和上下文资源登记，
不永久保活历史代码。结构类型根必须递归覆盖数组、集合、函数和异步类型中的名义身份。
描述和查询索引的原生存储参与现有内存记账，释放最后一个所有者后能够回收。

脚本中的描述对象通过正常 GC 边追踪所属状态和结构元数据，不为每个托管描述额外注册
永久外部根。程序全局持有描述、描述指回程序形成的普通对象图仍须可被 GC 回收。
独立持有的 C++ 描述才按外部资源协议注册根，不能把两种所有权合并成会阻止停止的根环。

新增反射 C++ 资源接入已有停止与外部资源释放协议。完整 Reload 创建新 Isolate，
宿主先释放旧反射句柄和根，再在新环境绑定；不以原始数值 ID 继续访问新对象。
仅包含复制字符串、数字和显示记录的工具快照可以独立保留，但不能用于旧环境的执行。
反射 ID 不承担跨运行存档、重命名迁移或网络实体身份。

提交后的变更摘要应在现有 `DevelopmentResult` / `DevelopmentChanges` 通知中补充登记、
成员结构和撤回信息，并携带既有 generation 与 revision。元数据消费者据此失效缓存，
不新增一套可能早于代码提交发布的通知通道。

## 10. 编译产物、AOT 与实现责任

| 责任方 | 必须完成的生产链路 |
| --- | --- |
| 前端与登记输入 | 解析规范模块和具体类型，绑定显式选择，验证成员种类与泛型约束；输出确定的登记事实 |
| SemanticModel 与声明绑定 | 提供真实所有者、继承解析、访问器聚合、完整类型与签名、默认存在性及稳定身份 |
| 泛型特化 | 保留登记的具体类型与方法特化，沿现有特化机制检查闭包，不在 runtime 请求新机器码 |
| Typed IR 与共享布局描述 | 携带登记关系、逻辑成员及原生目标/接口选择器，保持结构类型和声明关联 |
| 原生映像生产与消费 | JIT 内存描述、AOT 常量描述、实例化、ABI 身份和元数据追踪同时接通 |
| runtime | 共同的身份解析、查询索引、权限、类型检查、受检查访问与所有权 |
| AOT 可达性分析 | 保留登记元数据、签名和可执行入口的真实闭包，正确重映射局部索引 |
| JIT 缓存与代码复用 | 登记、查询可见事实和目标变化进入对应指纹；命中仍受源码检查与当前发布约束 |
| SDK 与 C++ | 同一契约的两种绑定、正确保根和失败传播；不能仅完成其中一侧 |
| LSP、doc 与 REPL | 从同一标准声明获取签名和文档；REPL 使用其真实开发会话上下文 |

逻辑属性需要前端提供独立且稳定的聚合身份，不能由 runtime 根据 getter/setter 的当前
编号或字符串临时合成。访问器本身继续使用各自的声明身份。新增这一事实应从统一声明
绑定生产者生成，并检查是否需要扩展当前的声明种类和映像记录。

现有成员记录主要服务动态执行，缺少完整稳定身份、静态成员和访问器可见性等反射事实。
实现必须沿权威声明和布局图补齐这些关联；允许建立面向查询的索引，不允许由多个消费者
分别维护名称、类型、签名或默认值镜像，也不能从调试名称或类型字符串恢复已丢失的结构。

反射目录是 AOT 的显式可达性根。即使没有普通直接调用，已登记可调用的方法、公开访问器和
构造也必须保留；纯 private 描述不因可见而获得可执行权限。
原生默认构造、字段初始化器、宿主导入和接口实现按真实依赖继续追踪。
只取得结构 `TypeInfo` 不应因此保留程序全部函数；已有 dynamic 边界所需的保留范围仍按
其原契约处理，不通过删除动态支持来缩减产物。

无源码的 AOT 可执行文件、目标文件和静态库必须提供相同反射行为。
反射执行不链接编译器或 LLVM，也不从源码、Clang AST 或临时文件恢复描述。
禁止在 AOT 缺失目标时自动切到 JIT 或按现有值猜测另一入口。

## 11. 性能与资源要求

编译准备按登记类型、成员和结构图规模建立索引，遍历递归类型时去重，不能展开成无限树。
名称和身份查找应使用哈希或有序索引，达到均摊 O(1) 或 O(log n)；批量枚举按结果规模线性生成。
确定性排序可在准备阶段付出 O(n log n)，不在每次字段访问前扫描全部程序声明。

可重载查询缓存必须包含发布版本或等价失效条件。成员缓存不能把跨安全点的字段地址或
退休代码地址作为永久结果。元数据枚举允许分配独立结果数组，但不能实例化业务对象。
所有可能分配前的接收者、实参、临时结果及结构类型借用须按现有根契约保活。

第一阶段不承诺反射访问等同直接字段访问的成本。完成时以 Release 测量类型/成员查找、
字段读写、调用、构造、元数据分配与反复重载后的保留资源，并对照直接访问和已有动态访问。
同时检查未使用反射的程序是否增加无关描述、执行开销或产物保留；结果遵守仓库的
[基准留存规则](../../.agents/benchmark_results.md)，不能用静态推断代替实测结论。

## 12. 实施阶段与验收

| 阶段 | 交付范围 | 完成条件 |
| --- | --- | --- |
| M0：契约冻结 | 显式登记用户入口、完整公共签名、身份与权限、错误 code、C++ 承载与上下文 | 第 13 节的必决事项已确认；登记和发现均有真实生产入口 |
| M1：描述与查询 | 登记生产者、类型图、稳定逻辑属性身份、全部描述、确定性枚举和查询索引 | 类型与成员事实从前端经过映像到 SDK/C++ 查询；只查询不执行用户代码 |
| M2：访问与调用 | 字段、静态槽、属性、方法、接口、构造、参数及 native class | 受检查语义、异常与 GC 闭环；两侧绑定及基线/优化 JIT 均可执行 |
| M3：AOT 与缓存 | 可达闭包、局部编号重映射、独立分发、SDK 标准声明、缓存和工具 | 无源码 AOT 可执行、目标文件和静态库通过；缓存命中和未命中语义一致 |
| M4：重载与生命周期 | 原子目录发布、当前句柄、增删与撤回、通知、停止、完整 Reload、元数据回收 | 保留状态操作正确；旧句柄不误绑定；描述不永久保活历史代码或已删除字段 |
| M5：完整验收 | 相关 Debug/Release 回归、工具检查、仓库静态检查和 Release 测量 | 下表覆盖完成；平台结果及未验证范围据实记录，不以局部通过宣称全部完成 |

| 验证面 | 必须覆盖的行为 |
| --- | --- |
| 登记与发现 | 未登记/空登记、错误目标、同名不同模块、规范 URI、别名、具体泛型、撤回及仅反射可达类型 |
| 类型结构 | null、void、dynamic、可空、空强类型容器、递归图、枚举名义身份、接口、函数和 Future 载荷 |
| 查询 | 本类/继承、实例/静态、可见性、真实声明者、未登记覆写、访问器配对与独立权限、确定性顺序 |
| 字段与属性 | 完整值类型、不可读写属性、private 拒绝、静态惰性初始化、写屏障、getter/setter 副作用与异常 |
| 调用与构造 | 有序具名/位置、重复与未知实参、缺失与 null、rest、枚举装箱、隐藏接收者、默认值仅执行一次、void 与 Future |
| 宿主边界 | C++ 查询与操作、无标签类型发现、`Type<T>` 桥接、跨 Isolate 拒绝、根、重入、绑定类寿命与关闭 |
| AOT 与缓存 | 无普通调用的反射入口、只描述与可执行闭包区别、裁剪及索引重映射、缓存命中/变化、无源码分发 |
| 重载 | 函数体替换、字段重排/新增/删除、旧句柄、同名重建、撤回登记、旧帧/闭包/Future、失败候选不发布 |
| 资源 | 只留描述不留旧代码、元数据根递归保活、重复查询与重载后回收、外部资源阻塞与完整 Reload |

实施时必须检查新增反射服务是否留下无生产调用方的旧辅助 API、重复签名解释、错误枚举和
临时注册路径。清理范围限于已被正式实现替代的部分；仍承担动态调用、类型化宿主调用或
普通构造职责的实现不属于历史残留。

## 13. M0 必决事项

以下内容尚未由 API 列表决定，是实现前需要确认的输入契约，不以本文示例代替决策：

1. **显式登记的用户入口**：确定编译/项目输入的实际表示，以及嵌入编译、CLI、AOT、开发会话
   如何传入和更新它。采用同一个声明绑定结果；本阶段不为此先引入特性语法或运行时注册回退。
2. **登记选择的精度**：确定字段读/写、属性访问器、构造、枚举成员和具体泛型方法的选择表达，
   保证只描述的 private 成员与公开可执行入口能够区分。类登记不得默认包含全部公开成员。
3. **C++ 公共承载签名**：冻结 `Reflection`、描述句柄和实参视图的完整签名与所有权，接入
   `Type<T>` 及现有外部资源类型。本文第 8 节规定行为，不将尚未存在的 C++ 声明当成既成实现。

这些事项确定后同步更新本文件和验收用例。特性系统以后可以成为登记输入的生产者之一，
但不能改变已确认的反射身份、权限和执行契约。

## 14. 参考

- [.NET Type](https://learn.microsoft.com/en-us/dotnet/api/system.type?view=net-10.0)
- [.NET FieldInfo](https://learn.microsoft.com/en-us/dotnet/api/system.reflection.fieldinfo?view=net-10.0)
- [.NET PropertyInfo](https://learn.microsoft.com/en-us/dotnet/api/system.reflection.propertyinfo?view=net-10.0)
- [.NET MethodInfo](https://learn.microsoft.com/en-us/dotnet/api/system.reflection.methodinfo?view=net-10.0)
- [.NET ParameterInfo](https://learn.microsoft.com/en-us/dotnet/api/system.reflection.parameterinfo?view=net-10.0)
- [Java Class](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/Class.html)
- [.NET Native AOT](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/)
- [架构与契约](../../.agents/architecture.md)、[泛型](generics.md)、[原生嵌入](native_embedding.md)、[原生类绑定](native_class_bindings.md)
