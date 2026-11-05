# C++ 类绑定与头文件生成设计

> 状态：标记、双端生成、`external class`、宿主对象运行时、JIT/AOT 和开发工具已实现。
> 验收范围、性能证据要求及平台状态见第 13 节。

C++ 头文件是宿主类接口的唯一来源。用户标记需要暴露的类，生成器通过 Clang AST 读取公开声明，
从同一份绑定描述生成 C++ 适配入口、显式注册函数和 Vela 模块。用户无需另写注册表或维护一份
Vela 类型声明。

类方法接入现有强类型宿主调用链。对于稳定对象、标量参数和标量返回值，性能目标是达到等价
手写宿主函数绑定的水平；相对直接接收 C++ 指针的原生调用，包装对象主要增加固定位置的指针
读取。逻辑帧、GC 根、错误和重入仍遵循[原生嵌入](native_embedding.md)及
[原生执行契约](native_execution.md)，实际成本按第 12 节验收。

## 1. 使用方式与交付范围

宿主在自己的头文件中声明类型：

```cpp
#include <cstdint>
#include <vela/bindings.h>

namespace app {

class VELA_CLASS("app.Counter") Counter final {
 public:
  explicit Counter(std::int64_t initial) noexcept;

  VELA_NAME("add")
  std::int64_t Add(std::int64_t delta) noexcept;

  VELA_GETTER("value")
  std::int64_t value() const noexcept;

  VELA_IGNORE
  void DumpDebugInfo() const noexcept;
};

}  // namespace app
```

生成器输出 `generated/app.vela`：

```vela
export external class Counter = 'app.Counter' {
  constructor(initial: int);

  add(delta: int): int;

  get value(): int;
}
```

`scripts/main.vela` 使用普通导入和对象语法：

```vela
import '../generated/app';

func main(): int {
  const counter = Counter(initial: 0);
  counter.add(3);
  return counter.value;
}
```

只需要默认名称时，类上保留一个 `VELA_CLASS` 即可；方法、构造参数名和签名从 AST 获取。
默认名称区分大小写，生成器不把 `Add` 自动改成 `add`，也不把名为 `getValue` 的普通方法
自动解释为属性。

本设计交付公开构造、实例方法、`const` 方法、静态方法、显式属性访问器，以及宿主对象在
脚本变量、字段、数组、集合、可空值、`dynamic` 和绑定方法值中的使用。JIT、AOT、REPL、
热重载和源码工具必须消费同一套类型与调用契约。

首版不包含 C++ 模板导出、脚本继承宿主类、C++ 继承关系到脚本继承关系的映射、运算符映射、
公开数据字段直绑、按值传递任意 C++ 对象、脚本回调自动封装或可被宿主随时销毁的借用对象。
这些接口若出现在待导出的公开声明中，应产生诊断并要求明确排除，不得静默生成另一种语义。
未标记的普通 C++ 声明不受影响。

实现复用 `NativeFunction::Bind`、不可变绑定快照和原生导入校验。
[`NativeType`](../../runtime/native/native_type.h) 统一表达逻辑种类、宿主类型键和可空性；
[`CanInlineImportCall`](../../compiler/native/host_call_emitter.cc) 接受已验证并保根的宿主引用；
[`ObjectKind::Host`](../../runtime/value/object.h) 提供独立的宿主对象存储和 GC 释放路径。

## 2. C++ 标记契约

新增公共标记头文件 `include/vela/bindings.h`，开发构建和发行包均以 `<vela/bindings.h>` 使用。
这个头文件只包含宏，不依赖 runtime、LLVM、Clang 头文件或注册对象：

```cpp
#pragma once

#if defined(VELA_BINDGEN)
#define VELA_DETAIL_ANNOTATE(text) [[clang::annotate(text)]]
#else
#define VELA_DETAIL_ANNOTATE(text)
#endif

#define VELA_CLASS(name) VELA_DETAIL_ANNOTATE("vela.class:" name)
#define VELA_NAME(name) VELA_DETAIL_ANNOTATE("vela.name:" name)
#define VELA_GETTER(name) VELA_DETAIL_ANNOTATE("vela.getter:" name)
#define VELA_SETTER(name) VELA_DETAIL_ANNOTATE("vela.setter:" name)
#define VELA_IGNORE VELA_DETAIL_ANNOTATE("vela.ignore")
```

正常编译时标记展开为空，不改变对象布局、继承关系或运行时行为。生成器在独立解析进程中
定义 `VELA_BINDGEN`；业务构建不得依赖该宏改变类的其他声明或实现。

| 标记 | 适用位置 | 契约 |
| --- | --- | --- |
| `VELA_CLASS("app.Counter")` | 类定义 | 指定全局宿主类型键；最后一段 `Counter` 是模块内导出名 |
| `VELA_NAME("add")` | 方法或参数声明 | 指定脚本名称；不改变 C++ 符号 |
| `VELA_GETTER("value")` | 实例方法 | 无显式脚本参数、返回非 void，生成属性读取 |
| `VELA_SETTER("value")` | 实例方法 | 一个显式脚本参数、返回 void，生成属性写入 |
| `VELA_IGNORE` | 构造函数、方法或数据成员 | 明确排除该声明 |

类型键必须非空、为合法 UTF-8，采用点分的 Vela 标识符，各段不得为保留字。键是公开名义身份，
不从 C++ RTTI 名称、编译器修饰名、文件路径或内存地址推导。不同 C++ 类型不能登记同一个键。

生成器只收集标记类自身声明的公开成员。private/protected 成员不导出，继承成员不自动复制到
脚本接口。C++ 类可以具有基类，已声明方法仍按真实 C++ 语义调用；脚本侧只观察注册的名义类型，
不据此推断 C++ 动态类型或父类关系。

复制/移动构造、赋值运算符和析构函数属于 C++ 生命周期机制，不生成脚本调用入口。其他公开
构造函数经排除后最多保留一个；零个表示不可从脚本直接构造。重载方法必须通过 `VELA_NAME`
形成不同脚本名称，或明确排除多余声明；不增加运行时重载选择，也不为此扩展 Vela 普通函数重载。
构造接口只收集源码中明确声明的构造函数，包括显式 `= default`，不额外导出 C++ 隐式构造函数。

导出方法和构造函数必须满足 `noexcept`，生成代码再次做编译期校验。构造对象需要可访问且
不抛异常的析构路径。首版拒绝默认实参、可变参数、模板类及其依赖嵌套类、纯虚方法、`consteval`
方法或构造函数、显式对象参数方法及公开数据字段直绑。
无法映射的公开声明应使用 `VELA_IGNORE` 排除，或由宿主提供满足契约的普通 C++ 接口。

参数必须在头文件中具有有效且不重复的脚本名称；必要时在参数上使用 `VELA_NAME`。访问器
标记按去除隐式接收者和可选 `NativeContext&` 后的脚本参数检查。getter/setter 类型必须一致，
只有 getter 的属性只读；重复标记、冲突名称和非法标记位置均报生成错误。

`const` 只约束一次 C++ 方法调用，不把脚本对象变成深度不可变对象。构造或方法可以将
`NativeContext&` 作为第一个显式参数，沿用当前宿主函数的隐藏上下文规则；该引用仅在调用期间有效。

## 3. 生成工具与构建输入

开发命令为 `python -m tools.vl bindgen --config bindings.json`：

```json
{
  "module": "app",
  "headers": ["host/counter.h"],
  "translation_unit": "host/bindings_input.cc",
  "compilation_database": "out/debug/compile_commands.json",
  "output": "generated",
  "dependencies": []
}
```

配置中的路径相对于配置文件所在目录解释。`translation_unit` 是已进入宿主编译数据库、包含
目标头文件的真实翻译单元。
编译数据库必须为该翻译单元选定唯一命令；存在多个目标或配置时需要显式分开配置，不能任选一项。

生成器使用 Clang LibTooling 及结构化 AST 接口，按选定命令的工作目录、头文件搜索路径、宏、
语言标准、目标架构和 SDK/sysroot 解析。仅移除编译输出和缓存包装等驱动项，加入生成器宏及
语法检查选项；参数用结构化 argv 传递，不把命令交给 shell 执行。不用正则解析 C++，也不消费
供人阅读的 `-ast-dump` 文本作为正式接口。

显式 `headers` 限定根声明集合，包含依赖参与解析和增量失效。对同一声明通过多个 include
到达的情况，按 Clang 的规范声明身份去重。引用另一生成模块的宿主类型时，需要显式提供该模块
的绑定清单依赖，使用可选的 `dependencies` 路径数组配置，由生成器产生普通相对 import；
缺少标记或依赖的类型直接诊断。

GN 配置默认导出 `compile_commands.json`，[`tools/clangd.py`](../../tools/clangd.py) 复用该数据库。
[`tools/clang_sdk.py`](../../tools/clang_sdk.py) 从 `DEPS` 固定的 LLVM 21.1.8 发布包提取
Clang AST/Tooling 开发头、静态库和 resource headers，并校验归档摘要。
生成器使用该 SDK 的 LibTooling；业务 C++ 编译仍使用仓库固定的 Fuchsia Clang 23 工具链。
编译参数由 LibTooling 驱动解析，不支持的参数或目标产生诊断。Clang 依赖仅进入
[`vela_bindgen`](../../compiler/bindgen/BUILD.gn)，不进入 runtime 链接依赖。

普通宿主编译器只需理解展开为空的标记。头文件生成要求受支持的 Clang 解析配置；不支持的
编译选项或目标明确报错，不改用另一个解析器或丢弃影响语义的参数继续生成。

生成工具作为构建动作，输出依赖文件并登记全部实际读取的头文件、配置、依赖清单及工具身份。
头文件变化自动触发重新生成。`--check` 仅验证输入和产物是否一致，不写入文件；陈旧或缺失
产物返回失败。脚本编译和语言服务读取已生成模块，不在查询过程中编译或执行宿主程序。

```powershell
python -m tools.vl build debug default
python -m tools.vl bindgen --config bindings.json
python -m tools.vl bindgen --config bindings.json --check
python -m tools.vl bindgen --verify-manifest generated/app.bindings.json
```

GN 集成使用 [`vela_native_bindings`](../../build/native_bindings.gni)：调用方提供 `config`、
四个 `outputs`（Vela、C++ 头/源和清单）、`depfile`，以及配置生成和依赖绑定的 `deps`。
配置的输出目录必须与 GN 声明一致；生成的 C++ 源文件进入宿主目标，目标依赖生成动作及
`//runtime`。完整配置示例见[生产测试绑定目标](../../compiler/bindgen/BUILD.gn)。

## 4. 统一绑定描述与生成产物

管线为：

```text
C++ 头文件及准确的编译配置
  → Clang AST
  → 规范化绑定模块
      ├─ C++ 类型化入口、类型描述与显式注册函数
      ├─ Vela AST、格式化后的 external 模块
      └─ 构建清单及依赖信息
```

规范化模块拥有类型键、导出名、成员种类、参数名、逻辑类型、可空性、构造信息、所有权转换、
上下文需求、C++ 调用目标、注释和源位置。类型引用使用规范身份，不能由各输出器分别推断。
C++ 类型声明使用 Clang 的声明和类型打印能力生成；Vela 输出构造生产 AST，并使用扩展后的
格式化和检查接口验证。字符串及路径使用对应的结构化转义规则。

| 产物 | 消费方与职责 |
| --- | --- |
| `app.vela` | 前端、JIT/AOT、LSP、doc、fmt、REPL；可独立静态检查的公开接口 |
| `app_bindings.h` | 声明显式的模块注册函数；业务宿主负责调用 |
| `app_bindings.cc` | 编译进宿主的适配入口、所有权编码、类型描述与注册实现 |
| `app.bindings.json` | 构建阶段的版本、目标、输入摘要、生成代次和各产物摘要；不是运行时类型表 |
| `app.d` | 宿主构建系统的增量依赖 |

注册函数返回带原因的显式结果，复用并扩展现有注册错误契约；类型和方法以一组原子登记，失败
不会留下半个类的注册。登记发生在实例化之前，并进入 `FreezeBindings()` 的不可变快照。
不通过全局静态构造器、进程级可变注册表或运行时扫描头文件完成注册。

示例模块的注册入口为 `vela::bindings::module_app::Register(isolate)`，返回
`std::expected<void, vela::runtime::NativeRegistrationError>`。清单的 `registration` 字段提供
实际入口名；模块名经合法 C++ 标识符编码，不要求宿主自行推导。生成头同时提供
`NativeClassTraits<T>`，供手写自由函数使用同一宿主类型 codec。

输出顺序由类型键和规范成员身份确定，不受哈希迭代顺序影响。清单记录两类摘要：公开契约摘要
用于接口比对；完整生成摘要覆盖 C++ 输入、配置、依赖和工具链。私有实现变化可以要求重建
C++ 产物，但不必改变公开类型身份。

全部产物先在临时位置生成并校验，成功后再替换，清单最后提交。多文件替换不宣称具有文件系统
单次原子事务；构建消费方必须比对清单与各文件摘要，中断造成的混合代次直接拒绝使用。
生成错误不发布候选结果，陈旧产物也不能作为成功生成的替代路径。
Clang 实际读取的文件先复制为不可变解析缓冲区，记录其 XXH3-128 摘要；发布前复核输入快照
及 SHA-256。C++ 头/源通过生成代次的 `static_assert` 耦合，文件加载器在捕获生成模块时
校验代次标记及全部产品摘要。已捕获的内存源码快照继续遵循 SourceSession 的不可变契约，
后续编译不重新读取磁盘清单。

## 5. 类型映射与可失败结果

标量规则保持现有宿主函数的严格类型边界，不隐式缩窄或转换数字种类。标记类记作 `T`，
其对应 Vela 名义类型同样记作 `T`：

| C++ 类型 | 使用位置 | Vela 类型及行为 |
| --- | --- | --- |
| `bool` | 参数、结果 | `bool`；沿用布尔 ABI codec |
| `std::int64_t` 的实际规范类型 | 参数、结果 | `int`，固定有符号 64 位 |
| `double` | 参数、结果 | `double`，binary64 |
| `void` | 结果 | `void` |
| `std::string_view` | 参数 | `string`；UTF-8 存储只借用到本次调用结束 |
| `std::string` | 结果 | `string`；经现有分配和 UTF-8 检查 |
| `std::optional<std::string>` | 结果 | `string?` |
| `CancellationToken` | 参数、结果 | 沿用现有共享取消状态 codec，脚本类型为 `CancellationToken` |
| `T&`、`const T&` | 参数 | 非空 `T`；调用期间借用底层对象 |
| `T*`、`const T*` | 参数 | `T?`；null 传空指针，否则调用期间借用 |
| `std::unique_ptr<T>` | 结果 | `T?`；非空结果将唯一所有权交给包装对象 |
| `std::shared_ptr<T>` | 结果 | `T?`；非空结果由包装对象保留共享所有权 |
| `Local<L>` | 参数、结果 | L 表达完整逻辑类型，包括容器、函数、Future 和生成的脚本名义类型 |
| `std::optional<Local<L>>` | 参数、结果 | 对应可空脚本引用 |
| `std::expected<R, vela::Error>` | 结果 | 成功类型按 R 映射；失败保留脚本异常载荷及诊断 |

直接公开的构造调用成功时产生非空 `T`，不会用 null 表示分配失败。`unique_ptr` 首版只支持
标准删除器；自定义释放方式必须先形成明确且可编码的所有权契约。智能指针不按 C++ 对象布局
跨原生 ABI 传递，入口仍使用固定宽度标量、运行时包装指针和显式状态。

首版拒绝裸指针/引用返回、`T` 按值参数或结果、转移所有权的参数，以及智能指针参数。
特别是不能从 `T*` 返回值猜测宿主是否转移所有权。宿主已有对象可以通过真实的共享所有权
进入脚本；空操作删除器不能代替生命周期保证。

`int`、`float`、无符号整数、`char*`、任意 STL 容器和未标记类型不会自动映射。
别名按 Clang 解析后的真实类型判断；不能只看 typedef 拼写。未来扩展必须同时定义数值、
空值、所有权及 ABI 契约，再扩展统一 codec。

自由函数与生成类绑定使用同一套类型化 codec。Future 的载荷和函数签名由
`Local<Future<T>>`、`Local<Function<R(Args...)>>` 明确表达；生成器按 Clang AST 解析嵌套
类型及别名。跨调用保存回调必须显式调用 `Persist()`，不自动扫描宿主字段。

构造和可失败静态工厂可以分别使用公开构造函数，或返回
`std::expected<std::unique_ptr<T>, vela::Error>` 的普通静态方法。生成器不执行构造函数、
默认表达式或任何宿主函数来确定签名。

## 6. Vela 声明与静态语义

`export external class Counter = 'app.Counter'` 引入宿主名义类型。类体仅包含无函数体的
构造、方法和访问器声明；不声明脚本字段、初始化器、类型参数、基类或脚本方法实现。
宿主类无隐式字段构造：未声明 constructor 就不能写 `Counter(...)`。

构造函数遵循现有具名构造实参规则，普通方法沿用现有参数绑定、求值顺序和命名参数规则。
`static` 方法通过类型名调用。绑定模块中的类型必须显式 export；模块别名、可见性和冲突
处理继续遵循[模块系统](modules.md)，不引入新的全局宿主命名空间。

前端把宿主类型键作为其专有名义事实保留到 SemanticModel、IR 和无指针映像描述中。成员引用
得到确定的绑定符号和签名，不能在后端根据名称重新寻找方法。相同键的重复定义、冲突成员和
同名不同宿主类型在静态阶段诊断；多个消费者应导入同一个生成模块。

生成描述使用以下成员绑定键。前端由类键、成员种类和脚本名称形成相同的键，原生后端只消费
已经绑定的结果；实际调用使用实例导入槽，不重新拼接键：

| 声明 | 绑定键示例 |
| --- | --- |
| 构造 | `app.Counter::constructor` |
| 实例方法 `add` | `app.Counter::method:add` |
| 静态方法 `Create` | `app.Counter::static:Create` |
| 属性 getter/setter | `app.Counter::get:value` / `app.Counter::set:value` |

C++ 适配函数的链接符号可使用生成器内部命名；它与上述公开绑定键分离，不依赖原始成员函数
的编译器修饰名。绑定键的编码和公开契约摘要使用同一规范实现。

`T?`、`is`、`as`、普通类型别名、以 T 为元素的容器及泛型应用沿用现有类型系统。
`dynamic` 调用和转换执行同一名义类型及 Isolate 归属检查。方法提取 `counter.add` 生成普通
绑定可调用值，捕获并保活接收者；它不能只留下脱离包装对象的裸 C++ 指针。

getter/setter 参与生产语义模型和动态成员表，只读属性写入应在静态或动态实际边界拒绝。
C++ 数据字段不进入 Vela 字段偏移表，脚本工具也不能观察宿主私有内存布局。

宿主类型不能自动参加字段结构反射或 JSON 类解码。相关检查必须明确识别该类型种类，给出
不可自动解码的诊断；不能把它当作零字段普通 class 继续执行。现有 Mock 工具也不能把宿主
具体类当作可生成实现的脚本接口。

## 7. 运行时表示、身份与所有权

新增一种明确的宿主对象存储，接入现有 GC 堆及 `ValueKind::Object`，不改变盒装 Value 的
16 字节契约。它具有固定位置的宿主指针、Isolate 内的稳定名义身份和释放所需的所有权记录。
C++ 内存不作为脚本字段数组存储，不套用普通 class 的字段迁移或布局访问。

不可变宿主类描述记录类型键、公开契约、构造/成员入口和所有权操作；Isolate 为它建立本地
名义身份。`NativeBindings` 快照可以共享不可变描述，但不能共享某个 Isolate 的堆对象或
名义类型指针。运行时不依赖生成器的 AST、JSON 清单或 Clang。

包装对象发布后，其底层 C++ 指针保持稳定，直到包装对象回收。唯一所有权对象由生成的类型化
删除入口释放；共享对象由包装对象持有一次 `shared_ptr`。普通 `T&` 方法参数直接借用 T*，
不在每次调用时复制智能指针或增减引用计数。C++ 对象不得在包装对象存活时自行删除或被外部
强制释放，容器迁移也不得移动其存储地址。

每个 Isolate 维护弱包装索引。相同类型身份和同一仍存活的原生对象再次进入脚本时复用包装，
不改变现有对象身份比较和 Map/Set 键语义。索引不保根；回收时在释放原生所有权前移除记录，
防止地址重用命中旧对象。共享所有权重新编码时校验控制块一致，不能把不同控制块下的相同
裸地址合并，也不能把已有唯一所有权对象重新作为独立所有权接收。

构造路径在调用宿主代码前保根全部托管实参，并为原生存储申请堆预算。原生对象由 RAII 持有，
直到包装和所有权记录创建成功后才发布。原生分配失败、包装分配失败及结果校验失败均释放
候选资源，报告现有 OutOfMemory 或 native 错误；已经执行的宿主副作用不承诺回滚。
本版构造使用可失败的分配入口和 `noexcept` 构造，宿主的自定义分配方式不满足该契约时应
使用受查静态工厂。

原生对象和所有权记录参与现有堆预算及外部内存统计，不能只统计 GC 包装头。
`sizeof(T)` 由实际 C++ 编译的描述提供；额外缓冲区和容量型资源由宿主在所属 Isolate 上
显式报告。共享对象按每个 Isolate 持有的包装计入该 Isolate 的预算，统计口径不表示进程物理
内存去重。分配、失败释放、GC 回收及 Isolate 销毁必须配对更新计数。
构造适配器使用 `NativeContext::Construct<T>(...)` 保留预算并持有尚未发布的所有权；
`NativeContext::Own(...)` 接收构造结果或智能指针，
`NativeContext::ReportExternalMemory(object, bytes)` 更新已包装对象的额外内存计费。

GC 触发的唯一对象析构及共享引用释放在 Isolate 所属线程发生。共享对象的最终析构也可能由
宿主侧最后一个所有者触发，其线程由该所有者决定。释放路径必须 `noexcept`，且不得重入脚本、
分配 Vela 对象或依赖其他待回收对象的析构顺序。要求特定线程释放的资源由其 C++ 所有者显式调度。
GC 时间不保证确定性；文件、图形和系统资源可提供普通的显式关闭方法，包装对象仍持有有效
的 C++ 对象，其关闭后行为由宿主接口定义。

跨调用保留脚本值使用 `Persistent<T>`，并遵循 [C++ 互操作](cpp_interop.md) 的关闭和引用环约定。
自动生成不会追踪任意 C++ 私有
对象图，也不能收集由 shared_ptr 与外部根共同形成的引用环；本版不自动生成这类持有回调的
类型转换，宿主必须显式断开自己建立的外部保根关系。关闭、Reload blockers 和 Isolate
销毁顺序必须覆盖这些资源，不能通过隐藏永久根延长包装寿命。

## 8. 类型化入口与调用性能

成员函数指针是生成时的模板常量，不能强转为普通函数指针。入口由实际 C++ 编译器调用成员，
保留 `const`、虚函数分派和指针调整。例如，对示例方法生成的入口在语义上等价于：

```cpp
std::int64_t CounterAdd(app::Counter* self, std::int64_t delta) noexcept {
  return self->Add(delta);
}
```

静态方法复用普通函数入口。实例方法沿下列路径生成代码：

```text
编译期已绑定的成员及具体签名
  → 实例化后固定的导入槽
  → 从已验证并保根的包装对象读取 T*
  → 类型化 ABI 调用
  → 寄存器标量结果或现有受查结果通道
```

绑定入口在每次脚本函数调用及异步恢复时按现有导入缓存契约解析，不在每次方法调用中查名称。
类型、归属和所有权验证放在实例化、宿主值进入脚本及 dynamic 等真实边界。静态类型保证成立
后，内部方法调用不重复解析字符串、查类型注册表、构造参数数组或进行通用 Value 拆参。

`NativeFunction` 的签名使用能表达宿主名义类型和可空性的统一描述。
标量和字符串 codec 使用同一表达，类方法不维护第二套签名匹配和错误通道。
成员适配器与普通函数共享上下文推导、直接结果 ABI 和受查结果完成逻辑。

共享 native 后端允许宿主引用实参进入直接调用路径，同时把包装接收者
及其他存活托管值纳入根规划。返回 void/bool/int/double 的合适签名继续走直接 ABI；
构造、字符串、智能指针编码和 `std::expected` 结果走明确的受查 ABI。这是按结果契约选择
调用约定，不是先执行一个入口、失败后重新调用另一个入口。

所有宿主方法默认仍可能分配、触发 GC 和重入脚本，包括 const/noexcept 方法。调用者必须
保持根、逻辑帧、调用深度和源码位置；可重载模式继续登记宿主访问边界。不能为达到性能数字
删除这些保证，也不能因为参数是指针就设置错误的 LLVM 无副作用属性。

首版不提供 `VELA_LEAF`。头文件和 noexcept 无法证明不重入、不使用运行时及执行时间短；
未来若增加叶调用，需要先定义可验证的效果契约，并统一修改 IR、GC、调试和重入规则。
本设计的低开销目标在现有宿主调用语义下验收。

构造和宿主对象调用保留真实的外部效果，不能被普通脚本 class 的分配消除当作可拆分字段记录。
包装指针只有在其不变性及保根成立时才可提升；不能跨越寿命结束或把接收者所有者丢弃。
C++ 模板封装可以由宿主编译器内联，JIT 不据此假定能跨二进制内联 C++ 方法本体。

## 9. 注册、错误与 ABI 校验

模块注册和原生实例化分别承担实际边界：

| 边界 | 必须检查 |
| --- | --- |
| 生成 | 非法标记、重复名称、不支持的 C++ 类型/声明、不可访问生命周期路径、缺少依赖 |
| 编译生成的 C++ | 精确成员函数类型、构造可用性、noexcept、当前目标的布局及 ABI 类型 |
| 模块注册 | 类型键及符号冲突、整组描述完整性、绑定冻结状态 |
| 实例化 | 缺失类型/方法、公开契约及具体参数/结果不符、运行时 ABI 和目标不符 |
| 值进入脚本及动态调用 | Isolate 归属、名义类型、null、返回所有权和控制块一致性 |
| 分配与结果完成 | 堆预算、UTF-8、原生失败及返回载荷约束 |

生成诊断使用原始 C++ 文件的行列，说明声明、失败类型及冲突位置。输入失败不使用断言或
`NOLINT` 掩盖，也不自动忽略不支持的方法。运行时失败复用 native 错误和可捕获的现有异常
通道；不引入 C++ 异常跨越脚本执行栈。

公开契约摘要由统一逻辑类型、成员种类、名称、命名参数及可空信息计算。前端根据生成声明
建立预期契约，运行时与注册描述比对；所有权编码和具体 native 入口同时参与运行时绑定校验。
文件名相同、方法数量相同或裸指针宽度相同均不能替代完整校验。

映像只保存无地址的宿主类型键、签名及导入关系。实例化建立 Isolate 本地关系，不把 C++
对象布局、shared_ptr 布局、RTTI 值或成员函数指针字节写入映像。新增描述和存储形状必须
更新原生 ABI 指纹；不兼容的旧产物要求重建，不提供旧布局解释路径。
当前 native ABI 版本为 14。类的完整公开契约摘要由
[`base/native_binding`](../../base/native_binding.h) 统一计算；未被当前脚本调用的成员也参与
实例化校验，参数名变化同样导致不匹配。

## 10. 缓存、热重载与源码工具

源模块缓存覆盖生成声明和其依赖；C++ 构建缓存覆盖完整生成输入和实际编译配置。宿主绑定的
公开契约变化使相关编译结果失效，不能用相同脚本文件路径复用不兼容的调用代码。

同一开发会话的宿主绑定在冻结后保持不变。脚本 Hot Reload 可保留宿主包装对象，活动旧代码
继续由既有代码所有者保活。修改宿主类型键、签名、属性或所有权契约需要重新生成、重建宿主
并创建匹配的会话；普通脚本 Reload 不能替换进程中已有的 C++ 实现或迁移 C++ 对象内存。

绑定方法值、异步帧和宿主导出句柄必须保持包装对象及所需代码的寿命。`FreezeBindings()`
复制描述而不复制 C++ 对象；新 Isolate 不继承旧 Isolate 的包装或原生对象身份。关闭前的
外部借用登记沿用现有阻塞规则。

LSP、fmt、doc、语法高亮和 REPL 支持 external class、构造、静态成员及访问器。
LSP 签名和命名参数来自同一生成声明；普通定义跳转落到生成模块。C++ 原始位置和文档注释
保存在生成清单中，构建及生成诊断使用原始位置；首版不增加跨语言重命名协议。

生成文件视为只读接口产物。用户更改头文件后通过构建动作更新；语言服务不在查询中执行
bindgen，也不把编辑器缓冲区中的不一致候选产物发布给运行实例。静态检查不要求 C++ 注册
已执行，缺失绑定由实际实例化阶段报告。

## 11. 模块职责与发行集成

| 现有边界 | 完整实施面 |
| --- | --- |
| `tools`、GN、DEPS | bindgen 调度、编译配置选择、Clang SDK、增量依赖、确定性输出与清单检查 |
| 新增的独立生成器目标 | Clang AST 收集、规范化绑定描述、C++ 和 Vela 输出；不链接进 runtime |
| `compiler/frontend` | external class AST/语法、模块及名义类型、构造/属性检查、动态和 JSON 等消费者边界 |
| `compiler/ir`、`base` 描述 | 保留宿主类型、成员目标、调用效果及构造契约，序列化无指针映像描述 |
| `runtime/native` | 统一签名、成员适配器、整组注册、实例化校验、类型化调用及受查结果 |
| `runtime/value`、`heap`、`isolate` | 宿主包装种类、弱身份索引、所有权、计费、扫描/释放及关闭顺序 |
| `compiler/native`、JIT/AOT | 接收者指针提取、根规划、直接调用、动态调用、闭包和异步存活 |
| 工具及发行包 | 模块生成集成、源码工具支持、公共标记头、生成器和干净宿主验收 |

发行包新增 `include/vela/bindings.h`，保留既有 `include/runtime`、`include/base` 路径。
生成源代码包含实际需要的 runtime 公开头；标记头本身保持独立。生成器是可单独调用的开发工具，
已经生成并编译的独立 AOT 应用运行时不需要 Clang、生成清单或源码编译器。
发行包携带 `bin/vela_bindgen`、Clang resource headers、格式化工具和
`share/vela/bindgen.json` 摘要清单。生成命令为
`python <package>/bin/vela-bindgen.py --config bindings.json`，要求宿主提供 Python 3.11+ 和准确的
C++ 编译配置；不要求安装仓库的 Python 开发模块或 Clang 开发库。

原生可执行文件仍按[现有 AOT 外部导入限制](aot.md)执行。本功能的 AOT 验收使用嵌入宿主链接
生成的 obj/staticlib 和运行时，由宿主显式注册绑定；不借此引入通用动态库装载、任意宿主符号
自动发现或另一套 AOT 链接配置。
宿主类出现在 AOT 导出签名时，继续使用现有带类型的保根句柄；T* 只在类型化绑定入口内部
借用，不能通过导出包装把裸指针当作独立拥有的脚本结果。

## 12. 性能验收

所有性能结果来自 Release，区分首次实例化/构造、预热后的调用和回收成本。
比较四种实现：直接的 C++ 函数指针调用、接收相同对象的手写 C++ 适配入口、使用同一 host
codec 的 Vela 自由函数绑定，以及生成的 Vela 类方法绑定。两种 Vela 路径必须保持相同错误、
GC、重入和追踪语义。两个纯 C++ 基线用于分离原生调用与适配本身的成本，不包含 Vela 运行时
边界，必须单独报告，不能据此宣称整个跨语言调用没有成本。

| 工作负载 | 必须记录或验证 |
| --- | --- |
| 空方法、整数累加、double、bool、void | ns/call、吞吐、指令形状；防止整个循环或结果被优化删除 |
| 同一对象热循环、交替多个对象 | 接收者提取、入口解析次数、方法调用中的额外分配及引用计数操作 |
| getter/setter、静态方法、绑定方法值 | 与等价自由函数或普通绑定调用的成本和语义一致性 |
| 构造、unique/shared 返回、重复包装、GC | 构造耗时、分配次数、峰值计费、回收后保留量及析构次数 |
| 失败、动态接收者、重入、宿主触发 GC | 正确性及必要检查的独立成本 |
| JIT 基线/优化层、封闭 AOT、可重载会话 | 分开报告，不将单一模式结果推广到全部路径 |

热路径不得按名字查找、创建参数容器、分配包装或复制智能指针。相对等价自由函数绑定，
类适配不能额外引入分派层；相对原始 T* 调用，解释固定的包装指针读取及运行时边界成本。
查看目标汇编确认适配器是否内联或尾调用，保留真实虚调用和必需的类型/寿命边界。

类方法与等价自由函数绑定的差异应落入重复测量的波动范围；超出范围必须定位到具体额外
工作并处理，不用大方法体掩盖绑定开销。文档不预设绝对零成本或未经测量的固定纳秒数字。
结果按[Benchmark Result Retention](../../.agents/benchmark_results.md)保存，包含机器、
工具链、配置、精确命令、重复次数和工作区状态。

完整套件命令为：

```powershell
python -m tools.vl bench --native-class-compare --report "$env:TEMP/vela-native-class.json" -- --benchmark_min_time=0.1s --benchmark_repetitions=7
```

套件包含 19 种工作负载、5 种执行模式的类/自由函数配对，以及 20 个 C++ 基线，共 210 项。
逐项校验结果；热方法和重复共享包装要求零分配，共享所有者数量在测量后恢复。
两侧初始化分别调用对应的类 setter 或自由函数 setter，避免不对称的导入槽改变循环优化。
实例化计时包含入口查找和接收者/全局初始化；所选工作负载的首次执行另计。
计时后先结束进行中的增量 GC，再按当前根执行完整回收，校验本轮构造与析构数量一致。
峰值及回收后内存包含弱索引容量；仍有全局包装存活时，保留索引容量不等于保留原生对象。
`dynamic` 配对分别经过动态成员查找
和动态 callable 校验，成本单独报告，不能据此承诺动态调用与静态调用具有相同开销。

需要检查代码时，将 `VELA_NATIVE_CLASS_CODE_CACHE` 设为临时目录，运行选定用例即可保存
JIT 的 LLVM IR、目标缓存和根/栈帧指标。生成适配器的目标汇编应为固定位置指针读取及直接
调用或尾调用；交替对象的 JIT 类/自由函数循环除导入和追踪编号外具有相同指令与根规划。
setter 配对在 JIT 两档中具有相同的规范化 LLVM IR。
空方法对比中，自由函数占据编号 0，LLVM 可合并零值帧记录的写入；类方法使用自己的非零追踪
编号。保留这些帧语义及其实际测量，不通过调整编号或删除追踪来追求数字相等。
机器码形状相同也不保证不同代码地址的纳秒测量相等，超出波动的结果仍须与指令及调用边界一起解释。

## 13. 实施阶段与完成条件

M0–M4 的生产路径已实现并在 Windows x64 验证。M5 的性能验收需要第 12 节完整原生类套件
及对应源码的测量证据；正确性检查通过不能证明调用开销符合目标。滚动摘要只保留最近两次
测量，当前记录的是其他套件，不能作为原生类绑定性能验收的依据。

| 阶段 | 交付 | 完成条件 |
| --- | --- | --- |
| M0：标记与生成输入 | 公共宏头、Clang 开发依赖、准确编译配置、AST 收集及规范描述 | 正常编译宏为空；生成正确保留类型/名称/位置；非法输入诊断完整；依赖版本固定 |
| M1：声明与双端输出 | external class 前端、C++ 入口/描述/注册、Vela 模块、清单与构建动作 | 同源确定性生成；脚本静态检查及工具识别；陈旧/混代产物拒绝；无手写重复注册 |
| M2：存储与所有权 | 宿主对象种类、唯一/共享所有权、弱索引、计费、失败清理和关闭 | 对象身份稳定；析构恰好一次；失败不泄漏；同地址/不同控制块及跨 Isolate 边界受查 |
| M3：执行与调用 | 统一 codec/签名、直接与受查 ABI、根规划、方法/属性/动态/绑定方法值 | JIT 各层和 AOT 均可执行；GC、重入、异常、深度及接收者寿命正确；无重复反射分派 |
| M4：开发与发行 | 缓存、Hot Reload/Reload、异步存活、LSP/fmt/doc/REPL、包与干净宿主 | 失效和冻结契约完整；旧代码/包装保活；离线 AOT 仅依赖所需运行时；生成工具不进入 runtime |
| M5：统一验收 | 全量回归、全仓库 lint、Debug/Release、Release 性能及平台验收 | 下表全部覆盖；第 12 节性能目标有证据；移除重复/废弃路径；明确未验收平台 |

| 验证面 | 必须覆盖 |
| --- | --- |
| 解析与生成 | 命名空间、别名、Unicode 路径、宏条件、包含去重、const/static/virtual、getter/setter、重载改名及忽略；错误标记/签名/默认参数/公开字段 |
| 增量构建 | 不同 target/配置、头文件和依赖变更、确定性输出、只读 check、缺失产物、生成中断、清单摘要不符 |
| 静态语义 | 导入及 export、命名参数、无隐式构造、成员冲突、只读属性、nullable、名义 is/as、容器和泛型引用、JSON/Mock 不支持边界 |
| 注册与实例化 | 冻结、组内/已有符号冲突、缺失类型/成员、参数及结果不符、旧 ABI、目标不符、失败不发布部分状态 |
| 对象寿命 | 构造/包装分配失败、unique/shared/null 结果、重复返回、控制块冲突、地址重用、原生内存计费、宿主根、关闭和析构顺序 |
| 执行 | void/bool/int/double、字符串 UTF-8、expected 错误、上下文参数、受查动态调用、绑定方法、递归深度、GC 压力、重入及异步帧保活 |
| 开发工具 | 生成类型补全/hover/命名参数/跳转、格式化幂等、文档生成、REPL、Hot Reload 保留对象、绑定变化需新宿主/会话 |
| AOT 与性能 | 脱离编译器的嵌入宿主、发行包标记头/工具、正确性一致的调用基线、目标汇编及第 12 节全部测量 |

平台矩阵为 Windows x64、Linux x64 和 macOS arm64，每个平台独立验收生成工具、原生运行时
及所需 SDK。未完成的目标明确记录为未验收，不能由其他平台编译成功推断。
按项目工作流在实现完成边界顺序执行构建、全量测试、全仓库 lint 和必要 Release 测量；
Windows x64 已通过 Debug/Release 构建、C++ 回归、工具测试及全仓库 lint。发行工具、
嵌入宿主、obj/staticlib 和脱离源码的运行均已验证。保留的性能测量以
[`rolling_summary.json`](../../benchmarks/results/rolling_summary.json) 的套件、源码和配置
为准。Linux x64 与 macOS arm64 尚未在本次实现中验收。

## 14. 参考实现与采用边界

| 参考 | 本设计采用的机制 |
| --- | --- |
| [Blink ScriptWrappable](https://chromium.googlesource.com/chromium/src/+/main/third_party/blink/renderer/platform/bindings/script_wrappable.h) | 脚本包装与宿主实例关联、稳定类型身份及包装复用 |
| [Blink 快速绑定生成器](https://chromium.googlesource.com/chromium/src/+/main/third_party/blink/renderer/bindings/scripts/bind_gen/interface.py) | 生成具体接收者提取和成员调用，避免通用拆参成为热路径 |
| [Blink NoAllocDirectCall 契约](https://chromium.googlesource.com/chromium/src/+/main/third_party/blink/renderer/bindings/IDLExtendedAttributes.md#NoAllocDirectCall) | 借鉴类型化快速调用；该名称中的 NoAlloc 是历史遗留，不能据此推断当前调用效果 |
| [Flutter dart:ui 的绑定声明](https://github.com/flutter/flutter/blob/main/engine/src/flutter/lib/ui/painting.dart)及[适配注册](https://github.com/flutter/flutter/blob/main/engine/src/flutter/lib/ui/dart_ui.cc) | 包装接收者、具体 ABI 和成员函数模板入口；自动生成部分由 Vela 工具补齐 |
| [Dart ffigen](https://github.com/dart-lang/native/blob/main/pkgs/ffigen/README.md) | 使用 Clang 解析头文件生成语言声明；该工具不直接支持 C++ 头文件，本设计需要自己的 C++ 声明收集 |
| [Dart leaf 调用契约](https://api.dart.dev/dart-ffi/Native/isLeaf.html) | 将调用效果作为独立契约；不从 noexcept 或类标记推导叶调用 |

这些参考不改变 Vela 的独立 Isolate、现有原生 ABI、精确根和显式失败模型。
