# 运行时反射设计

> 状态：已实现 `Reflect` 静态入口、`[Name]` / `[Name(...)]` 源码特性与按特性发现方法。
> 平台与验证范围见第 12 节。

Vela 的反射提供类型与成员描述、字段和属性读写、方法调用、对象构造及枚举查询。
`Reflect`、描述类型、枚举和参数对象由全局标准环境提供，无需导入模块。
通用查询使用 `Reflect.xxx()`，类型与成员操作使用对应描述对象的实例方法。
脚本与 C++ 宿主消费同一份编译产物元数据，JIT 与 AOT 使用相同的类型和执行契约。
成员是否公开与是否登记为可反射成员分别决定；未被显式选择的公开成员不会自动进入反射目录。

第 1 至 13 节定义反射执行契约和公共 API，第 14 节定义特性语法、继承关系和反射登记规则，
第 15 节定义按特性发现方法的接口。
编辑器控件、序列化、存档身份迁移、顶层函数与
全局变量反射、运行时开放泛型特化及动态代码生成不属于当前实现范围。
反射操作使用普通 API，源码标注使用统一的特性语法。

可运行模板见 [examples/reflection/main.vela](../../examples/reflection/main.vela)，
使用源码特性登记；[示例说明](../../examples/README.md#runtime-reflection) 包含运行和热重载步骤。

下面的例子通过类级 `[Reflect]` 显式选择 `Counter` 的公开成员，无需项目配置。

```vela
[Reflect]
class Counter {
  value: int = 10;

  add(amount: int): int {
    this.value += amount;
    return this.value;
  }
}

func main(): void {
  const counter = Counter();
  const info = Reflect.typeInfo<Counter>();
  const field = info.findField('value');
  if (field != null) {
    field.setValue(counter, 20);
  }

  const method = info.findMethod('add');
  if (method != null) {
    const arguments = CallArguments(items: [
      CallArgument(value: 5, name: 'amount'),
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

反射实现复用以下基础：

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
第 13 节的输入只登记类型时，不自动登记它的字段、方法、属性或构造；签名引用某个类型也
不自动登记该类型的成员。第 14 节的类级 `[Reflect]` 是显式批量选择公开成员的另一种表达。
继承查询只返回实际登记的成员，不因查询子类而扩大基类登记集合。

登记集合是编译输入，不由运行时执行某段注册代码后临时推断。AOT 的可达性分析、JIT 的
源码检查和开发会话的候选准备必须消费同一份绑定结果。当前登记入口与选择精度见第 13 节，
源码特性入口见第 14 节；
不能以内部测试手工构造的登记记录代替生产入口完成验收。

类型检查必需的结构信息与反射成员目录分开。`Reflect.typeInfo<T>()` 和 `Reflect.typeOf(value)` 可以描述
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

## 3. Reflect 静态入口与类型发现

既有五个通用反射入口统一定义为 `Reflect` 的公共静态方法，另新增第 15 节的按特性发现
方法接口。`Reflect` 与 `TypeInfo`、
`FieldInfo`、`MemberQuery`、`CallArgument`、`CallArguments` 等类型由全局标准环境提供，
无需 import。名称保护与局部遮蔽遵循[全局标准声明规则](standard_library.md)。
默认参数由标准声明与绑定共同实现。

通用查询只通过下表的 `Reflect` 静态方法提供，不保留同名全局快捷函数。
`Reflect.findMethods<T>()` 消费特性元数据与查询索引。

`Reflect.typeInfo<T>()` 使用现有静态泛型方法调用语法，由编译器将具体类型绑定到规范结构
描述。其余入口使用普通静态方法调用，不增加反射关键字。静态调用不构造 `Reflect` 特性
实例，也不为参数或返回值涉及的类型自动登记成员。编译器按实际引用提供所需标准声明；
全局可用不要求保留全部函数。反射成员的发现与访问范围仍由第 13、14 节的登记结果决定。

| API | 返回类型 | 行为 |
| --- | --- | --- |
| `Reflect.typeInfo<T>()` | `TypeInfo` | 获取静态指定类型的规范结构描述，不要求构造 T，也不登记其成员 |
| `Reflect.typeOf(value: dynamic)` | `TypeInfo` | 获取实际值的类型；数组和集合保留声明的元素类型，不按当前内容猜测 |
| `Reflect.getTypes(module: string)` | `TypeInfo[]` | 枚举当前程序中该模块登记的具体名义类型 |
| `Reflect.findType(module: string, name: string, typeArguments: TypeInfo[] = [])` | `TypeInfo?` | 按声明名及完整类型实参查找已登记类型，不在运行时生成新特化 |
| `Reflect.revision()` | `int` | 获取当前反射上下文的发布版本；用于识别查询结果所属发布 |
| `Reflect.findMethods<T extends Attribute>(module: string? = null)` | `MethodAttributeMatch<T>[]` | 查询当前程序中已登记且直接带有匹配特性的公开普通方法；模块为可选筛选，完整契约见第 15 节 |

入口与描述对象的职责按操作所属目标划分：

| 承载类型 | API 职责 |
| --- | --- |
| `Reflect` | 通用类型描述、类型目录发现、按特性发现方法和当前发布版本查询 |
| `TypeInfo` | 该类型的结构、赋值关系及成员目录查询 |
| `FieldInfo`、`PropertyInfo` | 目标字段或属性的描述与受检查读写 |
| `MethodInfo`、`ConstructorInfo` | 目标方法或构造的签名与受检查调用 |
| `EnumMemberInfo` | 目标枚举成员的描述、原始序号与值 |
| `MethodAttributeMatch<T>` | 一条匹配特性的数据副本及其所属方法描述 |

`module` 使用程序中已有的规范模块 URI。接口只查询当前程序图，不读取文件、解析导入、
执行模块初始化或加载新代码。未知模块的 `Reflect.getTypes` 返回空数组，`Reflect.findType` 返回 null。
导入别名、源码行号和内部特化显示名不构成查找身份。
`name` 是模块内未限定声明名；泛型实参通过独立类型描述传入，不在运行时解析类型文本。

透明别名按展开后的真实类型描述，不独立列入类型目录。泛型目录只列出已登记、已编译的
具体应用。对同一类型的多次登记合并为同一身份；相同模块和声明的不同具体类型实参保持区别。
类型枚举按声明名及规范类型实参键排序，不依赖哈希表顺序，也不将枚举位置作为持久身份。

`Reflect.typeOf(null)` 返回 Null 类型，`Reflect.typeInfo<void>()` 可用于描述无返回值的签名；
void 仍不是可存储的语言值。泛型函数可以转发 `Reflect.typeInfo<T>()`，具体特化负责产生
类型描述，抽象参数不进入运行时。

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
不把用户抛出的错误统一改写为反射失败。下表的两个反射专用 code 是独立的失败分类，
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

C++ 通过 `#include <vela/host.h>` 使用 `vela::Reflection`、`vela::TypeInfo` 与成员描述。
[公共头文件](../../include/vela/reflection.h)是完整声明的权威来源。
接口位于 `vela` 命名空间，以下按操作说明契约；参数与返回类型以头文件为准。
`Result<T>` 沿用现有宿主错误和根句柄契约。

| C++ 接口 | 契约 |
| --- | --- |
| `Reflection::Bind` | 绑定封闭实例时保留必要程序所有者；绑定开发会话时在操作中选择当前发布 |
| `Reflection::TypeOf` | 获取值的完整类型，要求同一 Isolate |
| `Reflection::GetTypes`、`FindType` | 枚举或查找已登记的具体类型 |
| `Reflection::Revision` | 获取当前发布版本 |
| `Type<T>::Describe` | 桥接已绑定的逻辑类型，不重新创建名义身份 |
| `TypeInfo::id` | 取得可复制的规范身份 |
| `TypeInfo::Describe` | 复制类型描述和具有类型根的关联句柄 |
| `TypeInfo::IsInstance`、`IsAssignableFrom` | 检查值的完整类型或类型赋值关系 |

`TypeInfo` 的成员查询保留脚本查询的范围、过滤和缺失语义：

| C++ 操作 | 契约 |
| --- | --- |
| `GetFields`、`GetProperties`、`GetMethods` | 按查询范围、存储种类与可见性枚举对应成员 |
| `FindField`、`FindProperty`、`FindMethod` | 按名称查找；泛型方法还匹配完整类型实参；合法缺失返回空值 |
| `GetConstructor` | 获取已登记的构造，合法缺失返回空值 |
| `GetEnumMembers`、`FindEnumMember` | 按原始声明顺序枚举或按名称查找枚举成员 |

成员描述继承 `MemberInfo`，共同提供稳定身份 `id`、描述副本 `Describe` 与类型查询 `GetType`。
`GetType()` 对字段和属性返回值类型，对方法返回结果类型，对构造和枚举成员返回所属类型。
成员查询返回具有明确元数据根的值对象，不返回跨安全点借用的地址。

| C++ 描述 | 操作契约 |
| --- | --- |
| `FieldInfo`、`PropertyInfo` | `GetValue`、`SetValue` 按所属 Isolate、接收者、权限与值类型检查后读写 |
| `PropertyInfo` | `CanRead`、`CanWrite` 检查公开访问权限；`GetGetter`、`GetSetter` 返回已登记的访问器，合法缺失为空值 |
| `MethodInfo` | 查询参数、返回类型、具体类型实参与异步标记；`Invoke` 使用调用方提供的接收者与有序实参 |
| `ConstructorInfo` | 查询构造参数；`Invoke` 使用有序具名实参创建实例 |
| `EnumMemberInfo` | `Ordinal` 返回原始序号；`GetValue` 返回保根的枚举值 |

`TypeDescription` 是独立记录，字段为 `id`、`name`、`module`、`kind`、`is_nullable`、
`is_registered`、`type_arguments`、`base_type`、`interfaces` 和 `signature`。
`MemberDescription` 包含 `id`、`name`、`declaring_type`、`kind`、`visibility` 和 `is_static`。
这些字段与脚本描述含义一致；可能缺失的值使用 `std::optional`，有序列表使用 `std::vector`。
`ParameterInfo` 包含 `name`、`position`、`type`、`is_optional`、`is_rest`、`has_default`；
`SignatureInfo` 和 `SignatureParameterInfo` 对应脚本结构签名，使用同样的 snake_case 字段名。
C++ 的成员种类名为 `ReflectionMemberKind`，其它查询枚举与脚本同名。

`CallArguments` 是 `std::span<const CallArgument>`。
`CallArgument(Local<Value> value, std::optional<std::string_view> name = std::nullopt)`
构造一条实参；位置实参省略 name，具名实参传名称。例如
`std::array{CallArgument(value, "amount")}` 可以直接传给 `Invoke`。
`CreateValue<T>(scope, value)` 通过现有 codec 创建保根的擦除值，结果为 `Result<Local<Value>>`。
调用和读取的成功结果、错误载荷在返回时均已保根。

仅绑定 Isolate 的基础类型、容器和函数类型也支持结构描述、类型判断及空成员查询。
脚本名义类型与宿主类的完整声明和成员目录需要绑定具体实例或开发会话。

从可重载 `NativeInstance` 绑定时必须采用其所属开发会话，不能借此将句柄固定到旧发布。
C++ 实参表示可以借用同一作用域内的名称和根句柄，其借用只覆盖调用期间，不能保留在
描述中。类型化便利接口继续通过现有 codec 编解码，不新增另一套宽松的值转换规则。
C++ 描述的复制、释放与操作遵守 Isolate 所属线程约束；跨线程工具仅接收复制的展示数据，
实际访问调度回所属线程。

脚本标准 API 与 C++ 层都调用共享 runtime 的反射解析、权限检查及执行服务。现有 `Type<T>`、
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
消费者需要组合多次纯元数据查询时，可以比较前后的 `Reflect.revision()` 并在变化时重新取得结果；
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
| 标准 API 与 C++ | 同一契约的两种绑定、正确保根和失败传播；脚本通用入口统一绑定为 `Reflect` 静态方法，相关类型接入全局标准环境 |
| LSP、doc 与 REPL | 从同一标准声明获取 `Reflect` 静态签名和文档，无需导入即可补全与使用；REPL 使用其真实开发会话上下文 |

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

运行 `python -m tools.vl bench --reflection-compare` 生成原始 JSON 和 Markdown 报告。
按模式、操作或查询规模筛选的方法及完整计时边界见
[反射基准说明](../../benchmarks/README.md#runtime-reflection)。

## 12. 实施阶段与验收

| 阶段 | 交付范围 | 完成条件 |
| --- | --- | --- |
| M0：契约冻结 | 显式登记用户入口、完整公共签名、身份与权限、错误 code、C++ 承载与上下文 | 第 13 节的编译登记输入契约已确认；登记和发现均有真实生产入口 |
| M1：描述与查询 | 登记生产者、类型图、稳定逻辑属性身份、全部描述、确定性枚举和查询索引 | 类型与成员事实从前端经过映像到脚本/C++ 查询；只查询不执行用户代码 |
| M2：访问与调用 | 字段、静态槽、属性、方法、接口、构造、参数及 native class | 受检查语义、异常与 GC 闭环；两侧绑定及基线/优化 JIT 均可执行 |
| M3：AOT 与缓存 | 可达闭包、局部编号重映射、独立分发、全局标准声明、缓存和工具 | 无源码 AOT 可执行、目标文件和静态库通过；缓存命中和未命中语义一致 |
| M4：重载与生命周期 | 原子目录发布、当前句柄、增删与撤回、通知、停止、完整 Reload、元数据回收 | 保留状态操作正确；旧句柄不误绑定；描述不永久保活历史代码或已删除字段 |
| M5：完整验收 | 相关 Debug/Release 回归、工具检查、仓库静态检查和 Release 测量 | 下表覆盖完成；平台结果及未验证范围据实记录，不以局部通过宣称全部完成 |
| M6：Reflect 静态入口 | 标准声明、绑定、泛型特化、IR、缓存、脚本调用方与工具 | 五个公共入口通过 `Reflect` 静态方法工作；旧全局入口移除；模板、测试、REPL、LSP/doc 和无源码 AOT 同步验证 |
| M7：源码特性与按特性发现方法 | 第 14、15 节的绑定、元数据、查询索引与脚本入口 | 特性数据经过完整编译链路；无模块实参的查询、配对结果、访问范围、继承匹配、AOT 与重载快照有真实生产路径和验收 |

| 验证面 | 必须覆盖的行为 |
| --- | --- |
| Reflect 入口与工具 | 无需导入即可使用五个静态方法、描述类型、枚举与参数对象；名称保护、局部遮蔽、静态泛型调用、LSP/doc 声明和 REPL 当前上下文 |
| 特性方法发现 | 不传模块即可查询；配对数据、可选模块筛选、实际特性类型、重复标注、声明去重、无构造副作用及重载后的快照与当前句柄语义 |
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

### 现有实现验收记录

2026-11-09 在 Windows x64 验证：Debug 覆盖 2,772 个相关 C++ 用例，Release 覆盖
2,779 个 C++ 用例。仓库静态检查覆盖 826 个 C++ 文件；随后新增的旧帧恢复用例也单独
检查通过。无源码可执行文件、目标文件和静态库验证了 `Reflect` 静态入口、特性查询及
现有 C++ 反射绑定，原生产物只链接独立 runtime。
Release 另通过 227 个工具与分发测试，其中包含 485 条既有语言用例的 AOT/JIT 对照。

新增验证覆盖直接数据与枚举装箱、重复配置、继承匹配、别名、模块排序、接口、泛型特化、
数据副本、缓存、撤回与删除、变更通知、GC，以及挂起旧帧在新发布中查询和调用。
Debug 命令行程序被现有 LSP 进程占用，因此本次未重新链接该程序及依赖它的 7 个基准测试；
对应命令行、分发和基准测试由 Release 覆盖。Linux 与 macOS 尚未执行验收。

Release 反射基准覆盖 `Reflect` 静态入口、方法发现与仅登记但不查询的对照。
复现命令、计时边界、模式差异、C++ 参考契约及统计方法统一见
[反射基准说明](../../benchmarks/README.md)。

未登记对照不创建反射索引或成员目录，但保留类型描述。只增加 `[Reflect]` 而不调用反射
API 的对照也不创建索引；首次反射查询建立索引，首次调用某成员时才物化其调用签名。
登记增加了映像数据与可达代码。资源场景验证原成员句柄调用当前实现，且每次 GC 后只
保留一份当前映像。

完整中位数、CV、分配量、映像大小、环境及命令保存在
[基准汇总](../../benchmarks/results/rolling_summary.json)。

## 13. 编译登记输入

CLI 从项目配置接收 `reflection` 完整登记数组。省略该键或提供空数组表示没有配置登记，
源码特性仍独立生效。每项包含
`module`、`type` 和可省略的 `members`。`module` 是相对 `vela.json` 的具体模块路径，
也可以是既有 SDK 或标准模块 URI；磁盘路径与普通导入共用路径解析，不接受通配符。
登记模块会进入编译图，类型可以仅由反射引用而没有普通调用。`type` 使用现有语言的
类型语法，在所选模块的词法导入作用域中绑定；例如 `Box<int>` 和 `Box<models.Value>`。
此语法只在编译期解析；运行时的 `Reflect.findType` 仍接收分离的名称与类型实参。

| 成员选择 | 字段 | 选择精度 |
| --- | --- | --- |
| 字段 | `kind: "field"`、`name` | 同时登记字段读写；private 字段仍只描述 |
| getter | `kind: "getter"`、`name` | 只登记该 getter，并登记所属逻辑属性 |
| setter | `kind: "setter"`、`name` | 只登记该 setter，并登记所属逻辑属性 |
| 方法 | `kind: "method"`、`name`、可省略的 `typeArguments` | 普通方法不带类型实参；泛型方法必须提供完整具体实参 |
| 构造 | `kind: "constructor"` | 不接受名称或类型实参，登记本类唯一构造 |
| 枚举成员 | `kind: "enumMember"`、`name` | 逐个选择，不自动包含其它枚举成员 |

`typeArguments` 是使用现有类型语法的字符串数组，使用与 `type` 相同的模块作用域。
选择只绑定类型自身声明的成员；继承成员需要登记其真实声明者。重复登记合并为同一身份，
不生成重复目录项。未知配置字段、错误 JSON 类型、未知声明、成员种类不符、开放泛型、
错误类型实参或违反上界均在编译边界失败，并报告配置位置。登记类型不隐式登记任何成员。

嵌入编译通过 `frontend::ReflectionSelection` 传入同一输入，类型文本可用
`frontend::ParseTypeSource` 转成生产 AST。JIT 的 `SourceOptions.reflection` 和开发会话的
`DevelopmentOptions.reflection` 拥有登记集合；源码检查与 AOT 使用相同加载、绑定和检查路径。
`SourceUpdate.reflection` 存在时完整替换该输入的候选登记，省略时沿用已提交集合；
空数组表示撤回该输入的全部登记。
该替换只作用于配置登记；源码特性由候选源码决定。移除一个登记来源时，仍被另一来源
选择的类型或成员继续保持登记。
失败候选不改变已发布集合，完整 Reload 保留所选择的登记输入并建立新运行环境。
登记配置原文属于候选的不可变诊断输入；泛型特化与延迟上界检查仍报告配置文件位置。
文件开发会话监视配置及仅登记引用的模块；配置语法错误不能使后续编辑沿用旧登记继续编译。

第 8 节和公共头文件定义 C++ 描述句柄、实参视图及其所有权。
第 14 节的源码特性是同一登记链路的另一生产者，不改变反射身份、权限和执行契约。

## 14. 源码特性与反射登记

源码使用 `[Name]` 或 `[Name(...)]` 在声明上附加特性。特性由普通类定义，沿用普通类型继承；
`Reflect` 是通用特性系统中的一个标准类型。普通标注不要求配套 `vela.json`。
第 15 节定义从特性出发发现方法的查询入口。

### 14.1 Attribute 与 Reflect

`Attribute` 和 `Reflect` 由全局标准环境提供，用户通过普通 `extends` 定义派生特性。

| 类型 | 继承关系 | 职责 |
| --- | --- | --- |
| `Attribute` | 所有特性的公共基类 | 标识可以用于源码标注的类型，提供统一的特性类型边界 |
| `AttributeUsage` | `class AttributeUsage extends Attribute` | 配置特性类型能否在同一声明上重复出现，`allowMultiple: bool = false` |
| `Reflect` | `class Reflect extends Attribute` | 标注时使声明进入反射登记集合，同时通过公共静态方法提供第 3 节的查询入口 |
| `Button` | 用户定义的 `class Button extends Reflect` | 继承反射登记含义，同时携带按钮名称等自定义元数据 |

`Attribute` 本身不触发反射登记。文档说明、编译检查等特性可以直接继承 `Attribute`，由各自
消费者解释；它们无需因为使用了特性语法而开放目标的运行时反射。`Reflect` 的派生特性则
同时具有反射登记含义，编译器使用已绑定的类型继承关系判断，不按 `Button` 等名称硬编码。

继承发生在特性类型之间。携带标注的业务类仍使用自己的父类和接口，无需继承 `Attribute`
或 `Reflect`。特性类型继承也不等于把某个标注自动传播到业务类的子类或覆写成员。

`[Reflect]` 表示无实参标注，`[Button(label: '重置')]` 的实参形状复用普通构造调用语法。
特性出现在声明前；表达式中的数组与索引继续使用现有方括号语义。该语法统一服务所有特性，
不为反射另设声明关键字。名称解析、别名和继承判断沿用现有类型系统。

同一个 `Reflect` 类同时承载特性身份与公共静态 API。`[Reflect]` 使用特性标注语义，
`Reflect.xxx()` 使用普通静态方法调用语义。`Button` 等派生类型的静态成员解析遵循普通
类继承规则；公共反射入口统一以 `Reflect.xxx()` 表达。

首版标注参数与字段默认值只支持直接的 bool、int、double、string、枚举值和 null，负数
使用普通一元负号。标注通过普通具名实参绑定到特性类的生成构造字段，遵守完整字段类型、
必需参数与省略规则；特性数据不执行用户构造函数或构造体，不支持函数调用、插值或容器值。
存在用户构造代码的特性类型不能用于首版标注。查询从已绑定数据物化副本，不在查询时补算默认值。

同一种具体特性默认不能在同一声明上重复标注；特性作者在类上显式配置后才能允许重复：

```vela
[AttributeUsage(allowMultiple: true)]
class Button extends Reflect {
  label: string = '';
}
```

重复判断使用标注类型的规范身份，不把不同派生特性当成同一种。`AttributeUsage` 仅用于特性
类声明，本身不允许重复；未在具体特性类上配置时使用 `false`。允许重复的记录保持源码顺序，
不合并参数相同的两条标注；业务继承不自动复制标注。

### 14.2 类级与成员级登记

类上的 `[Reflect]` 显式选择整个类的公开成员，不需要逐个重复标注：

```vela
[Reflect]
class Counter {
  value: int = 0;
  value2: int = 1;
}
```

编译器登记 `Counter`、两个公开字段以及自动生成的公开构造。批量选择范围为本类声明的
公开字段、访问器、方法和构造，包括公开静态成员；继承成员仍需由其真实声明者登记。
属性的 getter 与 setter 分别按可见性处理，公开 getter 不会顺带开放 private setter。

仅需部分成员时，可以只在成员上标注：

```vela
class Counter {
  [Reflect]
  value: int = 0;

  value2: int = 1;
}
```

成员标注同时登记所属类型，作为查找该成员的类型入口，但不扩大到其它成员。下表假定没有
其它登记输入额外选择该类型的成员，`info` 表示返回的 `Counter` 类型描述：

| 查询 | 类上标注 `[Reflect]` | 仅在 `value` 上标注 `[Reflect]` |
| --- | --- | --- |
| `Reflect.findType(module, 'Counter')` | `Counter` 的 `TypeInfo` | `Counter` 的 `TypeInfo` |
| `Reflect.getTypes(module)` | 包含 `Counter` | 包含 `Counter` |
| `info.isRegistered` | `true` | `true` |
| `info.getFields()` | `value`、`value2` 的描述 | 只有 `value` 的描述 |
| `info.findField('value2')` | 对应 `FieldInfo` | `null` |
| `info.getConstructor()` | 对应 `ConstructorInfo` | `null` |

登记范围不参与类型身份；两种写法描述的都是同一个名义类型。`Reflect.findType` 返回类型描述，
不构造业务对象。普通代码对字段、方法和构造的使用仍遵循现有语言规则。

没有类级或成员级反射标注、也没有其它登记输入的类型，不进入反射目录；`Reflect.typeInfo<T>()`
与 `Reflect.typeOf(value)` 仍可提供第 2 节规定的结构描述。成员级标注可以选择 private 描述，
实际操作继续报告 `ReflectionAccessDenied`，不因标注而改变访问权限。

### 14.3 自定义特性继承 Reflect

`Button` 使用普通类继承取得登记含义，业务代码只写一次用途明确的标注：

```vela
class Button extends Reflect {
  label: string = '';
}

class Counter {
  value: int = 0;

  [Button(label: '重置')]
  reset(): void {
    this.value = 0;
  }
}
```

编译器根据 `Button` 是 `Reflect` 的派生类型，为 `reset` 和所属 `Counter` 生成登记结果，
同时保留标注的实际类型 `Button` 及 `label` 数据。无需在 `reset` 或 `Counter` 上再加
`[Reflect]`，也无需为 `Button` 的类声明额外加反射标注。此例只选择 `reset`，不登记
`value` 或构造。

编辑器通过第 15 节的 `Reflect.findMethods<Button>()` 一次取得 `Button` 数据及对应方法，
据此决定按钮的显示与交互，点击时使用普通 `MethodInfo.invoke` 调用目标。反射服务负责
定位与受检查调用，按钮 UI 属于独立消费者；编译器只处理通用特性与 `Reflect` 类型关系。
本文不新增 `Button` 标准类型或编辑器专用调用规则。

### 14.4 编译链路与后续设计边界

前端在声明绑定阶段解析特性类型、目标和实参，把 `Reflect` 及其派生特性转换成显式登记
结果。类型级批量选择先展开成具体声明身份，再与第 13 节的项目或嵌入输入合并、去重。
后续 SemanticModel、Typed IR、映像、AOT 可达性与 runtime 使用同一结果，不另建运行时
注册通道。项目配置是独立的登记来源，不是源码特性生效的前置条件。

编译器只处理已有程序图中的声明；运行时 `Reflect.findType` 不因源码标注而加载未编译模块或生成
新的泛型特化。登记变化随现有候选事务一起发布；撤回登记、稳定身份、Hot Reload 与完整
Reload 的边界继续遵循第 9 节。登记与成员发现不执行特性构造函数、装饰回调或编辑器代码。

首版支持的标注目标与 `Reflect` 登记粒度如下：

| 标注目标 | `Reflect` 或其派生特性的登记结果 |
| --- | --- |
| 类声明，包括 `external class` | 类型及本类公开字段、访问器、已编译具体方法和可用的公开构造 |
| 类字段、访问器、方法或构造 | 当前成员及所属类型 |
| 接口声明、枚举声明 | 类型本身；成员仍需分别选择 |
| 接口方法、枚举成员 | 当前成员及所属类型 |

顶层函数、全局变量、类型别名与 import 不接受首版特性。非法目标、未知类型、非特性类型、
缺失或重复实参、非直接数据和违反重复规则都产生编译诊断。可空字段和 dynamic 字段没有
初始化器时，仍是生成构造的必需实参；需要可省略时显式声明 `= null`。

泛型声明上的标注随普通具体特化替换类型参数，模板本身也进行定义检查。类级批量选择和
泛型方法标注只作用于当前编译产生或显式保留的具体应用，不自行枚举类型实参。
仅供反射调用的具体应用可以通过第 13 节的编译输入选择。查询不在运行时生成特化。

语法、绑定、特化、映像、AOT、缓存、REPL、格式化、doc 与 LSP 使用相同的声明事实。
通用按目标读取特性接口及 C++ 特性查询的公共签名留待独立设计；第 15 节的方法发现接口
已经定义其脚本签名、继承匹配和快照契约。

## 15. 按特性发现方法

消费者以特性类型为查询条件，默认在当前程序中发现对应方法。一次查询同时返回方法描述
和标注数据，适用于按钮等工具消费者，无需先枚举模块、类型和方法，再逐个读取特性。
方法发现与普通成员查询共用登记、稳定声明身份及受检查调用。

沿用[第 14.3 节](#143-自定义特性继承-reflect)的 `Button` 和 `Counter` 定义：

```vela
func main(): void {
  const matches = Reflect.findMethods<Button>();
  for (const match in matches) {
    print(match.attribute.label); // 重置
    // match.method 是 reset 的 MethodInfo，调用时另行提供接收者。
  }
}
```

### 15.1 查询入口与范围

API 签名为 `Reflect.findMethods<T extends Attribute>(module: string? = null): MethodAttributeMatch<T>[]`。
`T` 必须是具体的特性类型；不满足上界或仍含未确定类型实参时，在编译期报错。

| 调用 | 查询范围 |
| --- | --- |
| `Reflect.findMethods<Button>()` | 当前反射上下文的所有已登记公开普通方法，不要求调用方知道模块名 |
| `Reflect.findMethods<Button>(module: moduleUri)` | 同一范围中声明模块等于规范 URI 的方法 |

省略 `module` 与显式传入 null 都表示不按模块筛选。未知模块或没有匹配项时返回空数组。
模块 URI 与第 3 节相同，按实际方法声明者的模块筛选，不使用导入别名或当前调用文件作为范围。

当前反射上下文与 `Reflect.revision()` 一致：封闭执行对应其程序图，开发会话对应其当前
发布。默认范围不跨其它程序实例或 Isolate，也不扫描工程目录、加载文件或执行模块初始化。
查询只消费已有编译产物，不因请求某个 `T` 而登记新方法或生成泛型特化。

方法范围沿用普通成员查询的公开访问约定，包含实例与静态方法。getter、setter、构造和
顶层函数不属于本接口的普通方法集合。显式登记的 private 描述不进入本接口的默认结果，
标注也不改变第 7 节的调用权限。接口方法仍按其已有 `MethodInfo` 选择器契约调用。

### 15.2 匹配与返回记录

`MethodAttributeMatch<T>` 由标准环境提供，无公开构造函数，包含两个只读属性：

| 属性 | 类型 | 含义 |
| --- | --- | --- |
| `method` | `MethodInfo` | 被该条特性直接标注的方法描述；保留真实声明者，不绑定业务对象 |
| `attribute` | `T` | 该条标注的数据副本，保留实际派生特性类型及其字段值 |

匹配使用普通类型赋值关系：实际特性类型为 `T` 或其派生类型时都可匹配。例如查询 `Reflect`
可以得到方法上直接使用的 `Button` 标注；查询 `Button` 不匹配只有 `[Reflect]` 的方法。
结果中 `attribute` 的静态类型为 `T`，实际名义类型仍是原特性类型，不重新构造成基类。

只有直接附加到方法声明上的标注参与查询。类上的 `[Reflect]` 可以批量登记其公开方法，
但不会为这些方法合成新的 `[Reflect]` 标注。基类方法被多个派生类型继承时，按真实方法
声明及标注记录只返回一次；覆写方法不会仅因基类方法有标注而自动成为匹配项。

每条合法的匹配标注产生一条结果，不按方法合并，也不擅自选择第一条标注。如果同一方法
允许附加多个匹配标注，结果保留每条数据；重复标注是否合法由第 14 节的特性使用规则决定。
结果按声明模块、声明类型及具体类型实参、方法名及具体方法实参排序，同一方法的匹配项
保持标注在声明中的顺序。数组位置与标注位置不作为持久身份。

返回数组是独立结果集合。每次查询为 `attribute` 产生独立的数据副本；修改该副本的普通
可写字段或修改数组，不改变编译产物中的标注、登记目录或其他查询结果。`method` 和
`attribute` 属性本身不能被调用方重新赋值。

### 15.3 查询、调用与重载

查询不构造 `Counter` 等业务对象，不调用目标方法、getter、特性构造函数或编辑器回调。
它可以分配结果记录和特性数据副本，数据来自前端已绑定并保留的元数据。第 14 节必须为
特性参数与默认值定义可复制的数据表示；不能通过在查询期间执行用户构造代码补齐数据。

查询结果不捕获接收者。调用实例方法时，消费者提供满足声明类型的对象；调用静态方法时
传入 null。实参、异步结果与异常继续遵循现有 `MethodInfo.invoke` 契约。查询不会将带参
方法改成无参方法，也不自动构造对象、执行方法或 await 结果。

一次查询选择并保活同一个当前发布，方法集合与标注数据均来自该发布。`attribute` 是查询
时取得的数据副本；`method` 是稳定身份句柄，后续调用仍解析调用时的当前发布。热重载修改
标注数据后，新查询得到新数据，旧副本保留原值；方法删除或撤回登记时，旧句柄仍按第 7 节
报告对应错误，不执行旧实现。

查询与调用的版本一致性、资源寿命和停止协议见第 9 节。

### 15.4 索引、AOT 与验证

前端在同一个声明绑定生产者中记录特性的实际类型、数据和所属方法稳定身份。该事实随
SemanticModel、Typed IR 和原生映像传递；查询层不从源码文本、显示名称或字符串标签反推
特性类型。第 14 节的登记结果和本节的实际标注记录是关联事实，批量登记不能伪造标注记录。

runtime 按特性类型及其继承关系建立索引，模块为可选过滤条件。查询从匹配的特性记录取得
目标，不在每次调用中扫描所有类型及其全部方法。结果生成成本按匹配记录和复制数据规模
增长；索引存储参与现有内存记账，随同一候选发布、失效与回收。

AOT 保留匹配标注所需的特性类型、数据布局、数据以及已登记可调用入口的真实闭包，正确
重映射局部索引。无源码的原生产物必须返回同样结果，查询不依赖编译器或 LLVM。缓存指纹
覆盖标注类型、数据、顺序、所属声明和登记变化。脚本入口使用共享 runtime 查询服务；
C++ 公共查询绑定的具体签名在第 14 节所列的后续设计中确定，不复制另一套索引或执行规则。

| 验证面 | 完成条件 |
| --- | --- |
| 基本使用 | 不传模块即可取得 `Button` 的方法与标签数据；无匹配和未知模块返回空数组 |
| 类型与匹配 | 泛型上界错误有编译诊断；派生特性匹配保留实际类型；类级批量登记不伪造方法标注 |
| 声明与顺序 | 同名不同模块、继承声明去重、覆写、合法重复标注及确定性排序有明确结果 |
| 查询边界 | 实例与静态方法、普通方法与访问器、private 范围符合契约；查询不运行用户构造或目标代码 |
| 数据与生命周期 | 修改结果不改变目录；重载后旧数据副本与当前方法句柄行为正确；GC、停止与完整 Reload 无历史代码根环 |
| 编译与工具 | 基线和优化 JIT、无源码 AOT、缓存、LSP/doc、格式化与 REPL 都消费相同声明和查询契约 |

本节不要求 `MethodInfo.getAttributes<T>()` 成为主要使用入口；通用的按目标读取特性 API
仍由第 14 节单独设计，方法发现消费者直接使用 `Reflect.findMethods<T>()`。

## 16. 参考

- [.NET Type](https://learn.microsoft.com/en-us/dotnet/api/system.type?view=net-10.0)
- [.NET FieldInfo](https://learn.microsoft.com/en-us/dotnet/api/system.reflection.fieldinfo?view=net-10.0)
- [.NET PropertyInfo](https://learn.microsoft.com/en-us/dotnet/api/system.reflection.propertyinfo?view=net-10.0)
- [.NET MethodInfo](https://learn.microsoft.com/en-us/dotnet/api/system.reflection.methodinfo?view=net-10.0)
- [.NET ParameterInfo](https://learn.microsoft.com/en-us/dotnet/api/system.reflection.parameterinfo?view=net-10.0)
- [Java Class](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/Class.html)
- [.NET Native AOT](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/)
- [架构与契约](../../.agents/architecture.md)、[泛型](generics.md)、[原生嵌入](native_embedding.md)、[原生类绑定](native_class_bindings.md)
