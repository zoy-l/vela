# 项目配置与测试框架设计

> 状态：T0–T5 已实现。本文定义通用项目配置 `vela.json`、面向 Vela 源码的测试库，以及 `vela test` 运行器。测试 API 和执行语义以 Dart `package:test` 为依据；配置复用已有 JSON 基础设施，测试路径必须显式提供。Debug/Release 回归、真实进程、静态检查与 Release 测量已在 Windows x64 完成；性能与资源证据见[滚动摘要](../../benchmarks/results/rolling_summary.json)。

> Mock 扩展状态：M0–M5 已实现。第 12 节定义 `sdk:mock`、`vela mock` 接口生成、配置、匹配、验证和异步等待契约，采用 Dart Mockito／Mocktail 的模型。Debug/Release、真实 CLI／LSP、静态检查和 Release 性能与资源验证已完成；测量边界见第 12.12 节及[滚动摘要](../../benchmarks/results/rolling_summary.json)。

项目在根目录通过 `vela.json` 声明测试入口：

```json
{
  "test": {
    "paths": ["tests/unit", "tests/integration"],
    "filename": "*_test.vela",
    "timeout": "30s",
    "concurrency": 4
  }
}
```

测试使用普通函数、闭包和异常，不引入测试专用语言语法：

```vela
import 'sdk:test';

func main(): void {
  group('整数运算', () => {
    test('加法', () => {
      expect(20 + 22, equals(42));
    });

    test('异步结果', async (): Future<void> => {
      const value = await Future.value(42);
      expect(value, equals(42));
    });
  });
}
```

同步和异步测试都使用 `test()`。`main()` 和 `group()` 创建测试树，运行器在注册结束后执行被选中的测试。源码模块必须显式导入测试库，运行器不向测试文件注入隐式全局名称。

## 1. 依据、边界与当前基础

### 1.1 与 Dart 的对应关系

| 项目 | Dart 的实际行为 | Vela 契约 |
| --- | --- | --- |
| 测试入口 | `main()` 注册测试，`test()` 接收同步或异步回调 | 保留；不增加 `testAsync` |
| 分组与生命周期 | `group`、`setUp`、`tearDown`、`setUpAll`、`tearDownAll`、`addTearDown` | 保留作用域、顺序及异步等待语义 |
| 断言 | `expect`、`expectLater`、可组合 matcher | 保留；测试断言独立于可被关闭的语言断言 |
| 配置 | 可选的 `dart_test.yaml` | 统一放入通用的 `vela.json` 的 `test` 节，只支持 JSON |
| 未指定测试路径 | `paths` 默认指向 `test/` | 不提供隐式目录；无命令行路径时必须配置 `test.paths` |
| 目录内的文件选择 | `filename` 匹配文件的 basename | 保留，扩展名使用 `.vela` |
| 命令行路径 | 替代配置的默认 `paths` | 保留；允许显式指定配置路径以外的测试文件 |
| suite 执行 | 普通执行模式使用独立 Isolate，suite 之间并发 | 每个 suite 独立 Isolate，suite 内顺序执行；不固定为每文件一个进程 |
| 异步错误 | 通过 Zone 归属测试，迟到错误可能使已完成测试失败 | 接通内部测试上下文与相同的错误归属行为，不新增公开 Zone API |
| 超时 | 无活动超时，可按测试、分组和 suite 配置 | 保留；不将其解释成进程硬终止或任务取消 |

Dart 当前 `test` 与生命周期回调使用 `FutureOr<dynamic> Function()`，并非只接受 `void` 或 `Future<void>`。Vela 保留“普通返回值忽略、Future 等待完成”的行为，不为测试框架增加 `FutureOr`、联合类型或函数重载；泛型约束和类型实参推导沿用[语言契约](generics.md)。

T0–T5 包括本机原生执行中的单元测试与集成测试、配置、发现、注册、生命周期、同步与异步断言、超时、标签筛选、并发及结构化报告。Mock 和接口生成另由第 12 节定义。浏览器测试、跨平台选择表达式、注解语法、快照测试、覆盖率、分片、重试和随机顺序仍不在范围内；不宣称完整兼容 Dart 的全部 API 或配置。

### 1.2 可复用的生产实现

| 现有实现 | 本设计中的职责 |
| --- | --- |
| [`JsonDocument` 与 `JsonOutput`](../../base/json.h) | 读取通用项目配置，写入机器报告；不引入 YAML 解析依赖或第二套 JSON 解析器 |
| [模块加载器](../../compiler/frontend/modules/source_loader.h) | 承载测试文件与 SDK 模块的同一依赖图、源码位置及缓存身份 |
| [标准环境](../../compiler/frontend/builtins/standard_environment.h) | 复用不可变 SDK 源码资源的构建和声明机制，区分内部 `builtin:` 来源与公开模块导入 |
| [函数调用签名](../../base/callable.h)与[运行时 callable 解析](../../runtime/value/callable.h) | 校验测试回调，保留闭包、绑定方法及所属程序的身份 |
| [`Future` 与 `FutureScheduler`](../../runtime/async/future.h) | 等待异步回调、推进微任务与 timer、观察未处理错误，传播测试上下文 |
| [CLI 入口](../../compiler/cli/vela_main.cc)与[`ProcessEntry`](../../runtime/process/process_entry.h) | 集成新命令与异步驱动，避免两套彼此抢占的未处理错误处理器 |
| [异常与源码栈](exceptions.md)、[源码工具链](source_tooling.md) | 定位注册失败、断言失败、异步失败和配置诊断 |

现有 `python -m tools.vl test`、GoogleTest 和 Python 工具测试继续验证 Vela 自身。新的 `vela test` 服务于 Vela 源码项目；两者拥有不同的使用者与执行职责，不相互替换。

## 2. 通用项目配置 `vela.json`

### 2.1 文件定位与共享读取

`vela.json` 是项目通用配置，不创建 `vela_test.json`、独立测试配置或 YAML 兼容入口。当前设计只定义其中的 `test` 节，不借此增加包管理、构建配置或任意自定义顶层字段。

`vela test` 从当前工作目录向上查找最近的 `vela.json`，命中的配置文件所在目录是项目根目录。找到后不再合并父目录配置。`--config FILE` 直接指定配置文件，路径相对调用时的工作目录解析，不再执行向上查找。

配置查找和解析属于共享的项目配置层，测试命令消费其类型化的 `test` 设置。不得在 CLI、语言服务和编辑器中各写一套 JSON 字段读取或根目录判断逻辑。当前没有项目配置需求的普通编译入口不因此强制要求 `vela.json`。

`vela test` 要求找到配置并包含 `test` 对象。无命令行路径时，`test.paths` 必须存在且非空；提供了命令行路径时，路径本身就是显式测试入口，可以省略 `test.paths`。两种形式都不隐式补入 `test/`、`tests/`、项目根目录或其它目录。

### 2.2 JSON 与字段校验

配置使用严格 JSON：UTF-8、单个根对象，不接受注释、尾逗号、重复键或未知字段。重复键由配置读取边界验证，不能沿用某些 JSON 使用场景中“后者覆盖前者”的语义。

通用配置层校验根对象和已注册的顶层节，各节拥有者校验内部字段。当前只有 `test` 节；将来其它功能增加配置时应登记自己的字段和校验器，测试层不能把整个项目配置解释为测试配置。字段的缺失、`null`、空数组和类型错误分别处理，不做字符串到数字、对象到数组等隐式转换。

配置诊断包含文件、位置、字段路径及原因，例如：

```text
vela.json:6:20: error: test.concurrency must be a positive integer
```

复用现有 JSON 解析结果与源文本定位信息；如果底层节点没有字段位置接口，在共享配置读取层建立必要的位置映射，不为定位重新实现 JSON 语法。

### 2.3 `test` 字段

| 字段 | 类型 | 契约 |
| --- | --- | --- |
| `paths` | 非空 `string[]` | 默认测试入口；每项为具体文件或目录，不能使用 glob。没有命令行路径时必填 |
| `filename` | 非空 `string` | 目录递归发现时使用的 basename glob，默认 `*_test.vela`；语法对齐 Dart 的 filename glob |
| `timeout` | `string` | 默认测试无活动超时，默认 `30s`；支持时长、倍数和 `none` |
| `suite_load_timeout` | `string` | 单个 suite 的编译和注册超时，默认 `none`；只支持绝对时长或 `none`，不与测试体计时混用 |
| `concurrency` | 正整数 | 同时执行的 suite 数；默认 `max(1, floor(逻辑处理器数 / 2))` |
| `reporter` | `string` | `compact`、`expanded` 或 `json`；Windows 默认 `expanded`，其它支持平台默认 `compact` |
| `include_tags` | 非空 `string` | 标签布尔选择表达式；缺省时不限制包含标签 |
| `exclude_tags` | 非空 `string` | 标签布尔排除表达式，排除优先 |
| `tags` | 对象 | 声明标签及其配置；值为配置对象，允许空对象。首版支持 `timeout` 和 `skip` |

除 `paths` 的条件必填外，其余字段可以省略。未知 reporter、不可表示的时长、非整数并发度、重复 JSON 键或非法标签表达式都在编译测试源码前报错。

配置中的路径相对 `vela.json` 所在目录解析，采用 `/` 分隔。命令行路径相对调用时的工作目录解析，接受平台原生路径写法。改变工作目录不能改变已经解析的项目根目录。

优先级为：内建非路径默认值、项目配置、适用的标签配置、外层分组、内层分组、测试自身。配置和命令行同时提供相同 runner 选项时，命令行覆盖配置；命令行 `--timeout` 替代项目默认超时，分组及用例的局部超时仍按继承规则应用。`--concurrency` 和 `--reporter` 不受源码局部配置影响。

timeout 的绝对值覆盖继承值，倍数作用于继承的时长。配置中的 `2x` 以标准 `30s` 为基准；分组中的 `Timeout.factor(2.0)` 作用于上层已经计算的时长。`none` 禁用对应范围的无活动超时，不关闭错误观察或用户取消。

标签配置中的 selector 使用与命令行相同的语法。多个匹配配置按 JSON 成员的源码顺序应用，随后应用源码中更内层的元数据；不依赖哈希容器的迭代顺序。

### 2.4 通用配置与编译缓存

项目配置由一次命令会话读取并发布为不可变快照，保存规范配置路径和已校验字段。命令执行期间不因文件修改而混入另一版本；后续命令重新读取。

测试选择、并发度与 reporter 不改变源码语义，不能直接使整份编译缓存失效。SDK 测试源码、可调用签名或宿主导入契约发生变化时，必须进入现有编译依赖指纹。以后其它配置节影响编译时，由该节生产者提供真正相关的配置依赖，而不是把 `vela.json` 原文无差别加入所有缓存键。

## 3. 测试发现与命令行

```text
vela test
vela test tests/unit
vela test tests/unit/math_test.vela
vela test tests/unit tests/integration
vela test --config path/to/vela.json
vela test --name "整数.*加法"
vela test --plain-name "整数运算"
vela test --tags "unit && !slow"
vela test --exclude-tags "network"
vela test --timeout 1m --concurrency 2
vela test --reporter json
vela test --list
```

位置参数提供一组具体文件或目录，整体替代配置的 `paths`，不与其求交或合并。目录内递归选取 basename 满足 `filename` 的 `.vela` 文件；直接指定文件时无需符合 filename 模式，但必须是可编译的 Vela 源文件。这与 Dart 的显式文件入口行为一致。

同一路径通过多个入口、相对写法或符号链接重复出现时，按模块加载器使用的规范文件身份去重。目录遍历不跟随目录符号链接，避免循环遍历；显式入口指向符号链接时按其实际文件身份处理。候选文件按规范的项目相对路径排序；不同卷等无法表示相对路径的情况使用规范绝对路径。同一个 suite 的注册顺序由源码实际注册顺序决定。

一次发现的结果在本次命令中固定；执行中创建的新文件不会加入。不存在或不可读的显式入口必须报错，不将其当作没有匹配文件。配置和显式入口都不能导致静默扫描未声明的其它目录。

`--name` 将正则表达式应用于完整测试名，`--plain-name` 使用字面子串；重复提供的名字过滤条件同时满足才入选。标签支持 `!`、`&&`、`||` 和括号，分组标签向后代继承。名字和标签过滤只决定执行哪些测试，不绕过文件编译和注册校验。

未发现 suite、注册后没有测试，或筛选后没有任何测试时，输出明确的空结果并以非零状态结束。全部入选测试都显式跳过时，按 Dart 的跳过语义成功结束，报告零通过及实际跳过数量，不把跳过记录计为通过。

`--list` 编译 suite 并执行 `main()` 和 `group()` 的注册逻辑，输出名称、标签、跳过原因和源码位置，不执行生命周期钩子或测试体。成功列出至少一个测试即可返回 `0`，不要求实际执行测试体。注册是普通程序执行，不能宣称 `--list` 或动态测试发现没有源码副作用。编辑器的静态符号查询不得在用户输入时隐式启动它。

## 4. SDK 测试模块与回调契约

### 4.1 显式导入

公开入口为 `import 'sdk:test';`。`sdk:` 使用独立的 SDK 模块解析分支，普通相对导入继续使用文件解析；不支持已删除的 `std:` 导入。

SDK 模块随 Vela 发行，使用不可变源码资源进入生产模块加载、绑定、类型检查、文档和缓存管线。内部 `builtin:` 来源继续表示标准环境内部身份，不能作为用户可覆盖的 SDK 文件路径。

本设计只登记 `sdk:test` 及其内部依赖，不扩展成包下载器或任意字符串到宿主代码的映射。解析器按 scheme 选择 SDK 或文件来源，不在 SDK 模块不存在时退回项目同名文件。SDK 导出与普通模块一样支持导入前缀、名字冲突诊断和模块私有性。

`check`、`run`、`vela test`、原生映像、内存编译和 LSP 必须消费同一份 SDK 源码与声明身份。`sdk:test` 不能只在测试 CLI 中识别，也不能由语言服务再造一份 stub。内存编译仍只从明确提供的源码集合和内置 SDK 资源加载，不增加隐式磁盘读取。

### 4.2 同名 `test` 接受同步与异步

Vela 没有 `FutureOr`。测试库的注册边界使用现有 `dynamic` 保存回调，并根据 callable 的真实签名校验；不增加 `testAsync`，也不把所有回调强制声明成返回 `Future<void>`。

`test`、setup/teardown 和 `addTearDown` 接受可用零个实参调用的非空 callable。存在可选参数时使用已有默认实参规则，不能包含必须由调用方提供的参数。它可以返回 `void`、任意普通值或任意现有 `Future<T>`。普通值忽略，Future 的完成值忽略但必须等待成功或失败；同步 throw 与 Future 失败均进入相同的报告通道。注册 API 的 body 参数为 dynamic，不提供泛型函数值所需的函数类型上下文；应传入已完成特化的函数值，或先通过函数类型标注完成推导。

`group` 的注册回调必须为可用零个实参调用、返回 `void` 的同步 callable，执行后立即恢复外层注册作用域。不能让 async group 返回的 Future 被忽略，导致后续测试注册到错误分组。

真实回调签名由[既有调用元数据](../../base/callable.h)提供，不通过试调用推断。非函数、要求必填实参的函数、空值、错误的 group 返回类型都在注册边界报告注册失败。第一版不因静态参数为 dynamic 而承诺与 Dart 完全相同的编译期回调错误诊断；运行时校验是明确的公开边界。

框架回调通过共享原生调用和 Future 归一化路径执行，保持所属 Isolate 与可调用对象的代码身份，也不能把脚本 `void` 伪装成可存储的 dynamic 值。需要继续执行脚本的操作应返回到所属执行驱动，由普通脚本帧或既有续执行机制完成。

### 4.3 公开注册 API

| API | 行为 |
| --- | --- |
| `test(description, body, ...)` | 注册叶子测试，description 为 string；可配置 `skip`、`timeout`、`tags` |
| `group(description, body, ...)` | 注册嵌套分组并同步调用注册体；可配置相同元数据 |
| `setUp(callback)` / `tearDown(callback)` | 登记当前分组内每个测试的前置与清理回调 |
| `setUpAll(callback)` / `tearDownAll(callback)` | 登记当前分组内一次性的前置与清理回调 |
| `addTearDown(callback)` | 在正在执行的测试或受支持的 setup 范围内动态登记清理 |
| `Timeout(milliseconds: int)` | 非负的绝对无活动时长 |
| `Timeout.factor(value: double)` | 正的有限时长倍数 |
| `Timeout.none()` | 禁用该范围的无活动超时 |

`skip` 接受 false、true 或说明字符串，`tags` 接受单个字符串或字符串数组；这类 Dart 风格的多形参数使用 dynamic，并在边界检查实际种类。`timeout` 使用 `Timeout?`。缺省元数据继承外层作用域。使用配置中没有声明的标签时，按 Dart 的行为报告警告，标签仍参与筛选；不把警告升级成另一套强制标签声明语法。

注册期允许同步 `main(): void` 或异步 `main(): Future<void>`；异步 main 完成前可继续在根注册范围添加测试。main 的 Future 只用于注册，运行器等待它完成才封存测试树。运行测试体后再次调用 `test`、`group` 或注册静态钩子属于错误，不能改变已经开始执行的测试树。

第一次注册时为 suite、group、test 和钩子分配稳定的会话内 ID，并保存真实注册调用位置。包装辅助函数可能影响定位时，应保留用户调用栈，不猜测函数名或源码行文本。完整名称由外层描述和叶子描述组合；同名测试允许存在，身份始终使用 ID，不使用名称作为唯一键。

## 5. 生命周期、筛选与失败

每个 suite 使用独立的 Program、Isolate、模块全局状态和测试注册树。suite 内顺序执行，文件之间才并发；不并发访问同一 Isolate。普通共享依赖被不同 suite 导入时，各自持有独立全局状态。

对一个被选中、未跳过的测试，按以下顺序执行：

1. 需要首次进入的分组按外到内执行 `setUpAll`。
2. 按外到内执行 `setUp`，同一层按注册顺序执行。
3. 调用测试体并等待其结果及框架登记的异步期望。
4. 执行本次测试的动态清理，再按内到外执行 `tearDown`；同一层按注册逆序执行。
5. 分组最后一个入选测试完成后，按内到外执行相应 `tearDownAll`。

所有回调逐个等待，不能使用 `Future.wait` 并行执行清理。后注册的动态清理先执行，清理回调抛错后仍继续执行其余已经登记的清理。动态清理中继续登记清理时按栈顺序处理，不遗漏或重复执行。

测试体或 `setUp` 失败后仍执行已建立范围内的清理。某个 `setUpAll` 失败会阻止该分组后代的测试体执行，报告前置失败及受影响测试，不制造后代通过记录；相应的 group 清理仍有机会执行。没有任何入选测试的分组不运行 `setUpAll` 或 `tearDownAll`。

`addTearDown` 在测试体或 `setUp` 中属于当前测试；在 `setUpAll` 中属于相应分组，并先于该分组的静态 `tearDownAll` 执行。不允许在无执行范围时登记动态清理。每个范围保存自己的清理栈，不用全局“最后一个测试”来归属。

保留首次失败和所有后续清理失败，分别标记阶段及源码栈；后续失败不能覆盖最初原因。跳过测试不运行其逐测试钩子，报告跳过原因。内部配置错误、suite 编译失败与注册失败是 suite 级错误，不伪装成某个断言失败。

## 6. 断言与 matcher

### 6.1 基本 API

`expect(actual, matcher, reason: null)` 同步匹配；传入普通期望值时，按 Dart 的 matcher 包装规则等价于 `equals(value)`。`expectLater(actual, matcher, reason: null)` 返回 `Future<void>`，用于需要等待的 matcher。`fail(message)` 产生专用的 `TestFailure`，不会因 Release 构建关闭而失效。

第一版提供以下 matcher：

| 类别 | API 与含义 |
| --- | --- |
| 值 | `equals(value)`、`same(value)`、`isNull`、`isNotNull`、`isTrue`、`isFalse` |
| 数值 | `closeTo(value, delta)`、`greaterThan(value)`、`lessThan(value)` |
| 字符串和集合 | `contains(value)`、`hasLength(length)`、`isEmpty`、`isNotEmpty`、`orderedEquals(values)`、`unorderedEquals(values)` |
| 组合与谓词 | `isNot(matcher)`、`allOf(matchers)`、`anyOf(matchers)`、`predicate(callback, description)` |
| 同步异常 | `throwsA(matcher)`，actual 为无参函数时同步调用并匹配异常 |
| Future | `completes`、`completion(matcher)`、`throwsA(matcher)`，actual 为 Future 时按异步 matcher 处理 |

组合 matcher 接受数组，适配 Vela 当前没有源码 rest 形参及调用展开的事实，不增加另一套变长调用语法。`same` 使用已有引用身份或基本值身份规则；`equals` 独立定义深度匹配，不能修改语言的 `==`。

数组按元素顺序匹配，Map 按键及对应值匹配而不要求插入顺序，Set 使用无序匹配。普通 class 默认按其既有相等/身份规则匹配，不自动反射私有字段或执行任意 getter。数值保持 Vela 的显式类型语义，不为断言引入 int/double 隐式转换。

`greaterThan` 和 `lessThan` 接受同种类的 int 或 double，种类不同则不匹配。`closeTo(value: double, delta: double)` 匹配 double，delta 必须非负且有限。`orderedEquals` 接受数组或 Set，按数组顺序或 Set 的迭代顺序匹配；`unorderedEquals` 对两者验证一一匹配。

`equals` 对数组、Map、Set 的递归匹配必须识别成对访问的循环容器，避免无限递归；一个分支共享引用不等于循环。使用显式工作栈和访问表，不设置随意的固定嵌套深度。无序匹配不能为缺少等价哈希的深度值强行套用语言容器哈希；涉及可重叠的自定义 matcher 时必须验证一一匹配，不能使用会误判的贪心选择，性能报告应记录这类匹配关系的实际比较成本。

对 Future 使用异步 matcher 时，`expect` 会为当前测试登记异步期望；`expectLater` 额外返回可供 await 的完成 Future。用户未 await 返回值也不会让已登记的期望脱离测试。普通 `expect` 不能声称自己已经同步确认了尚未完成的 Future。

### 6.2 自定义 matcher 与诊断

SDK 导出普通的 `Matcher` 接口和匹配结果类型，自定义 matcher 可以用 Vela 实现。同步接口为 `match(actual: dynamic): MatchResult`；异步接口 `AsyncMatcher` 为 `matchAsync(actual: dynamic): Future<MatchResult>`。`MatchResult` 保存成功状态、要求说明和可空的差异节点，差异节点保存路径、实际值说明及子差异，不将所有诊断提前拼接成单个字符串。`expect` 和 `expectLater` 根据实现的接口选择同步或异步流程；matcher 的源码调用通过普通脚本执行路径发生，使用共享原生调用服务。

深度匹配失败至少保留首个有意义的差异路径，例如 `items[3].name`，并提供实际值、期望说明和 `reason`。大型值的输出可以截断，但匹配本身不能因显示预算而提前判为成功。打印值不能任意调用业务 getter、`toJson()` 或其它有副作用的转换方法。

`TestFailure` 是普通可抛出实例，断言位置使用第一次失败的源码栈；框架帧可以在显示时折叠，原始结构化栈仍须保留。同步异常、任意自定义可抛出类和 Future 失败均匹配原始异常对象，不强制转换成全局 `Error`。

### 6.3 异步回调期望

提供 Dart 风格的 `expectAsync0` 和 `expectAsync1`，包装无参或单参回调，支持期望调用次数及允许的最大次数。单参版本的类型实参按 Vela 当前规则显式提供。回调不足时测试保持未完成直到超时，超出次数立即失败；包装回调抛出的错误保留测试归属。

其它参数数量的便捷包装不属于首版范围。测试可以通过普通闭包把已有回调适配到这两种形式，不增加源码 rest 形参。

## 7. 异步上下文、超时与收尾

### 7.1 测试上下文传播

Dart 依赖 Zone；Vela 当前没有公开 Zone。本实现增加内部、可选的测试执行上下文，包含 suite、测试或钩子范围、运行次数以及报告会话身份。上下文不保存用户对象裸指针，也不作为可枚举的普通脚本值暴露。

创建微任务、注册 Future 续延、创建 timer、挂起/恢复 async 帧和创建宿主完成端点时，必须保存其所属执行上下文，实际回调执行时恢复。Future 监听器使用注册监听器时的上下文，不能因为生产者稍后在另一测试期间完成就改归生产者或“当前测试”。错误封套保留真正产生错误的上下文和源码栈，转发时不重新归属。

这里覆盖静态和动态 Future 调用、闭包、绑定方法、错误回调、timer 回调与宿主完成消息。只在 `test()` 调用前后设置一个 Isolate 全局测试 ID 不能满足契约。没有测试上下文时保持普通运行路径，避免为所有正常 Future 建立测试注册对象。

运行器统一拥有会话期间的未处理错误观察。复用并调整现有 `ProcessEntry` 的观察和诊断职责，不能覆盖其 handler 后悄悄丢失另一类错误。错误已经被 `throwsA` 或用户 catch 正常处理时，不额外报告未处理失败；真正未处理的异步错误按调度器检查点通知。

测试体完成且已登记的异步期望结束后，可以开始清理和下一个测试。框架不等待该测试创建的每个普通 Future 或无限周期 timer；用户应通过 await、异步期望和清理明确管理工作。

迟到错误在 suite 仍存活时归属原测试，允许将已经报告完成的测试改判失败；不能算给随后开始的测试。suite 最终关闭后已经不存在可执行的脚本工作，宿主端点关闭，迟到的外部完成按现有 `Closed` 契约处理，不注入另一个 suite。

### 7.2 无活动超时

默认 `timeout: "30s"` 表示运行器确认的无活动时长，不是测试的总墙钟上限。进入测试或钩子、完成框架登记的异步期望，以及 `expectAsync` 包装回调产生有效进展时更新相应心跳；单纯轮询调度器、打印日志或无关的后台 timer 不算进展。

时长使用单调时钟。支持 `30s`、`1m 30s`、`2x` 与 `none`，零绝对时长表示下一次有机会检查时立即超时；负数、溢出、非有限倍数或非法单位报错。源码 `Timeout` 对象与配置字符串在归一化后进入同一时长模型。

测试体或异步期望超时后记录 `TestTimeout`，结束相应等待并进入清理阶段；清理也有自己的无活动计时，不能因测试体已经超时就无限等待。超时不取消原 Future，不撤销副作用，也不保证迟到工作停止，后续错误仍保留原归属。

与 Dart 的普通测试超时一样，这项功能不承诺抢占同步死循环、持续阻塞的宿主调用或无法得到调度机会的代码。不能仅调用 `Future.timeout()` 就宣称具有硬中止能力，也不引入每 suite 一个进程或终止 C++ 线程作为隐含实现。需要整个命令的强制墙钟上限时，由 CI 或调用方设置进程级超时。

suite 的编译和注册使用独立的 `suite_load_timeout`；该计时不属于某个测试，也不会通过日志或测试心跳重置。超过限制后停止接纳该 suite 的结果，发出 suite 加载失败；任务取消必须遵守实际编译器和调度器能处理的停止边界，不能谎称同步编译已被强制抢占。

### 7.3 生命周期与 GC

注册树拥有所有回调、matcher 和元数据。持有的脚本值使用已有精确根与写屏障；不能仅存放在未注册的 C++ 容器里。执行栈、挂起 Future、动态清理及错误对象在等待和清理期间保持可达。

上下文身份的宿主记录可以保留到 suite 报告完成，但不能因此永久保根整个 Program 和所有测试对象。结束 suite 时按现有异步端点关闭顺序停止接纳宿主消息、关闭调度工作、释放脚本句柄，再销毁 Program 和 Isolate。测试回调、导入绑定和诊断元数据必须覆盖各自消费者的生命周期。

## 8. suite 调度、错误与退出码

一个 runner 会话持有配置快照、候选 suite 列表、过滤条件、并发调度和报告聚合。每个 suite 的执行线程独占其 Isolate、注册树与事件循环；suite 之间只能传递已复制的报告数据，不共享 `Value`、Program 全局槽或 GC 对象。

可复用不可变的编译输入快照或已经验证可跨 Isolate 使用的现有编译产物；不能把在某个 Isolate 上实例化的闭包或全局存储直接交给另一个 worker。并行编译必须遵守现有编译缓存的原子发布契约。

用户取消后不再启动新的 suite，对活跃 suite 请求已有执行机制支持的停止并完成可安全执行的收尾。不能在任意指令或宿主借用期间销毁 Isolate，也不通过操作系统强杀线程。

退出码复用[现有工具错误分类](../../base/process_exit.h)：

| 情况 | 退出码 |
| --- | --- |
| 存在入选测试，所有测试通过或显式跳过，suite 无错误；或成功执行 `--list` | `0` |
| 断言、用户异常、测试超时、钩子/注册失败、未发现或选中任何测试 | `1` |
| 参数、配置格式或配置字段错误 | `64` |
| suite 源码语法或类型错误 | `65` |
| 运行器内部失败、运行时无法继续的资源错误 | `70` |
| 配置或显式测试入口 I/O 失败 | `74` |

出现多个类别时按运行器内部失败、I/O、配置/参数、源码、普通测试失败的顺序选择退出码，报告保留全部已知原因。断言失败不终止其它独立 suite；继续执行由会话调度器负责，不是重新运行失败测试或切换执行后端。

`vela test` 使用封闭 JIT 编译和共享原生运行时。每个 suite 创建独立 Isolate 与 NativeInstance，
suite 内按既有顺序执行；可重载层级和 AOT 不是该命令的可选测试平台。
SDK 声明、回调签名与错误上下文独立于前端 AST 生命周期。

## 9. 报告与工具链

所有 reporter 消费相同的结构化事件，不解析另一种 reporter 的终端文本。事件模型参考 Dart 的 `start`、`suite`、`testStart`、`print`、`error`、`testDone`、`done`，并使用 Vela 自己明确版本化的协议，不宣称与 Dart JSON reporter 二进制或字段完全兼容。

JSON reporter 按行输出完整 JSON 对象，stdout 只承载报告事件，运行器自身不可恢复的启动诊断进入 stderr。脚本的普通 `print` 转为带 suite/测试上下文的输出事件，不能破坏 JSON 流。每条事件包含会话内递增序号、单调相对时间和适用的稳定 ID。

协议标识为 `"protocol":"vela.test"`、`"version":1`，`sequence` 从 1 递增，`time` 使用会话开始后的单调毫秒数。定义事件 `group` 和 `test` 保存分组关系、标签、跳过原因及注册栈；执行事件引用 `suiteID`、`testID`、`scopeID`，钩子错误还包含 `hookID`。`--list` 的定义事件带 `listed:true`，不产生测试体执行事件。

错误事件包含阶段、错误类别、说明、源码位置、原始栈和可用的 matcher 差异。`testDone` 表示该测试的执行阶段结束，不能阻止迟到的 `error` 更新其最终状态。`done` 是会话最终判定，计数使用最终状态，不能继续沿用第一次完成时的通过计数。

`compact` 提供交互式进度，`expanded` 逐项输出结果，非交互终端不输出依赖光标移动的控制序列。并发 suite 的事件交错是正常行为；各 suite 内的生命周期顺序必须保持，显示名称始终附带可定位的文件身份。

LSP 补全、悬停、文档、导入链接与格式化消费真实 SDK 声明，不维护测试专用语法解析器。测试发现的权威结果来自注册执行，不能通过搜索 `test(` 文本来声称完整发现动态注册测试。VS Code 测试面板的独立 UI 集成属于后续工作，本次交付必须提供它可消费的列表和机器报告。

`vela run FILE` 直接运行测试文件时，复用 SDK 注册会话和同一执行驱动，在 main 注册完成后自动执行本文件的 suite。这条入口只执行显式文件，不执行配置发现或 suite 并行；缺少 `vela.json` 不影响已有的直接文件运行契约。与 Dart 直接运行测试文件类似，该模式不提供完整多文件 runner 的全部选项，但断言和异步错误不能变成无声成功。

## 10. 模块职责与实施阶段

以下职责由当前实现承担；阶段表同时作为后续维护的完成契约。

| 层次 | 所有者与职责 |
| --- | --- |
| 项目配置 | 编译器工具公共层持有 `vela.json` 定位、解析、节校验和不可变快照；底层 JSON 复用 `base` |
| SDK 源码与导入 | frontend 的模块生产路径登记 SDK 资源，模块绑定和工具查询消费同一声明 |
| 测试库 | SDK 源码实现公开注册包装、matcher 与用户可扩展接口；宿主桥接只承担真实的注册、诊断和调度边界 |
| suite 驱动 | 编译器工具层连接编译、注册、原生 callback 调用、Future 等待、生命周期和结构化结果 |
| 异步上下文 | runtime 持有与编译器无关的可选上下文与错误归属；原生执行保存、传播和恢复它，不把 runner 配置塞入 runtime |
| CLI 与报告 | CLI 处理参数，runner 调度 suite，reporter 展示结构化结果；避免所有逻辑堆入 `vela_main.cc` |

| 阶段 | 完成条件 |
| --- | --- |
| T0：通用配置与 SDK 模块 | `vela.json` 的共享解析和路径契约完成；`sdk:test` 在磁盘、内存编译、缓存和 LSP 中具有相同身份；没有 YAML 或独立测试配置路径 |
| T1：注册与同步执行 | main/group 注册树、同名测试身份、所有生命周期钩子、动态清理、异常与源码位置贯通；`test` 回调边界校验完成 |
| T2：断言 | 普通值包装、深度/循环集合匹配、自定义 matcher、异常匹配和结构化差异完成；不修改语言相等语义 |
| T3：异步完整契约 | 同名 test 等待 Future，expectLater/expectAsync、测试上下文、迟到错误、超时和异步清理贯通；GC 与宿主端点关闭验证完成 |
| T4：项目运行器 | 路径发现、过滤、suite 隔离并发、三个 reporter、退出码和直接 run 文件接通；机器报告可以独立消费 |
| T5：交付验证 | Debug/Release 回归、CLI 真实进程验证、语言工具和编译缓存验证、静态检查、性能与资源测量完成，同步实际支持文档 |

阶段是同一功能的实现顺序，不能在 T1 同步测试通过后将整个测试框架标记为完成。最终交付必须完成上述范围内全部生产路径，删除过程中形成的重复解析器、隐式注入、临时 callback 适配和独立错误 handler。

## 11. 验证与完成标准

配置验证覆盖根目录查找、显式配置、严格 JSON、重复键、未知字段、路径基准、无配置/无路径、命令行替代路径、非法超时和并发度；确认没有隐式默认目录。嵌套目录中的同一命令应解析到相同配置与同一候选集。

发现与注册验证覆盖直接指定非标准文件名、重叠目录、规范路径去重、非法入口、稳定排序、同名测试、循环生成测试、同步/异步 main、错误的 async group，以及 `--list` 的注册副作用和无测试体执行。

生命周期验证覆盖嵌套顺序、所有失败阶段、多个清理错误、动态清理递归登记、部分 setup 失败、全部筛掉的分组，以及跳过与无测试执行。每次失败后其它独立 suite 仍可正常运行。

断言验证覆盖基本值、显式数值类型、共享与循环集合、深层不匹配、无序集合、异常原对象、matcher 副作用、自定义 matcher 抛错及诊断截断。必须使用外部期望结果验证行为，不能只验证实现生成的字符串等于自身规则。

异步验证覆盖已完成/未完成 Future、同步 throw、首个 await 前失败、跨 await 的 catch/finally、多监听器、Future 采用、timer、宿主线程完成、expectLater 未 await、回调次数不足/超出、超时后的清理、迟到错误归属，以及下一测试开始后前一测试才失败。验证 idle 等待不会让有超时计时器的测试提前变成假死锁。

资源验证在注册、挂起、matcher、错误报告和清理期间触发 GC，检查所有回调和异常保根；suite 关闭后检查对象、挂起帧、timer 和宿主端点回收。测试 Isolate 之间不能共享模块可变状态。未启用测试框架的正常 Future 路径不得引入无用途的报告对象。

工具和真实进程验证覆盖 `vela test`、`vela run`、配置错误退出码、源码错误退出码、JSON 流有效性、脚本输出捕获、迟到失败后的最终计数、并发 suite 隔离，以及 SDK 导入的 check、LSP、文档、缓存和镜像身份。

实现完成边界执行相关 Debug/Release 回归、仓库静态检查和 Release 测量。测量小 suite 启动、批量 suite 编译缓存命中、注册/断言吞吐、深度匹配内存、并发资源峰值及普通异步程序回退；按[基准结果保留规则](../../.agents/benchmark_results.md)保存证据。只修改本文时检查内容、链接、UTF-8、LF 和 diff，不运行无关构建。

## 12. Mock 扩展

### 12.1 目标与代表性用法

提供独立的 `sdk:mock` 模块，支持接口替身生成、调用记录、返回值与异常配置、自定义响应、实参匹配和捕获、调用次数与顺序验证，以及异步调用等待。业务代码接收具有原接口类型的对象；测试通过生成的调用描述入口配置和验证它。公开 API 使用普通 Vela 声明，不增加注解、宏、源码 rest 形参或 `noSuchMethod` 语言机制。

例如，业务模块定义：

```vela
// lib/user_repository.vela
export interface UserRepository {
  load(id: int): Future<string>;
  save(id: int, name: string): void;
}
```

显式生成测试模块：

```text
vela mock lib/user_repository.vela --interface UserRepository -o tests/user_repository.mocks.vela
```

测试代码使用生成对象与调用描述：

```vela
import 'sdk:test';
import 'sdk:mock' as mock;
import './user_repository.mocks.vela' as generated;

func main(): void {
  test('读取并保存用户', async (): Future<void> => {
    const session = mock.MockSession();
    const repository = generated.MockUserRepository(session: session);

    mock.when(repository.calls.load(42)).thenAnswer(
      async (invocation: mock.MockInvocation): Future<string> => {
        return 'Alice';
      }
    );
    mock.when(repository.calls.save(42, 'Alice')).thenDo(
      (invocation: mock.MockInvocation): void => {}
    );

    const name = await repository.object.load(42);
    repository.object.save(id: 42, name: name);

    expect(name, equals('Alice'));
    mock.verifyInOrder([
      repository.calls.load(42),
      repository.calls.save(42, 'Alice'),
    ]);
    mock.verifyNoMoreInteractions(repository.control);
  });
}
```

`repository.object` 的类型为原始 `UserRepository`；实际项目将它传给被测服务。`repository.calls` 创建调用模式，不执行接口方法；`repository.control` 用于检查和管理该 mock 的记录。三者由生成的包装对象持有。业务接口可以拥有名为 `calls`、`object` 或 `control` 的方法，因为这些成员属于包装对象，不占用被替身接口的成员空间。

### 12.2 与 Dart 框架的对应关系

| 能力 | Dart 的实际做法 | Vela 契约 |
| --- | --- | --- |
| 接口适配 | Mockito 通常用 `build_runner` 生成 mock；Mocktail 使用 `Mock` 与 `noSuchMethod` | 静态生成真实接口实现与调用描述类型，复用生产模块图、绑定与类型检查 |
| 配置调用 | Mockito 使用 `when(mock.method(...))`；Mocktail 使用 `when(() => mock.method(...))` | 使用 `when(mock.calls.method(...))`，不运行实际方法或配置闭包 |
| 实参 matcher | Mockito／Mocktail 在真实参数位置接收 matcher 占位值；Mocktail 对自定义类型需要 `registerFallbackValue` | 调用描述参数接收普通值或 matcher，不需要把 matcher 转成业务类型或登记占位对象 |
| 非空返回 | Mockito 生成适当的录制返回值；Mocktail 的闭包处理录制期间的 TypeError | 创建调用模式时不求取业务返回值，不捕获并忽略类型错误 |
| 匹配多个 stub | Mockito 使用最后配置的匹配项 | 按同一方法的配置逆序匹配，最后配置优先 |
| 异步响应 | Mockito 要求用 `thenAnswer` 创建 Future／Stream 响应 | Future 方法使用 `thenAnswer`，在实际调用上下文中创建 Future |
| 验证消费 | Mockito／Mocktail 后续验证排除已验证调用 | 验证成功后消费对应记录；失败时不消费，批量顺序验证整体提交 |
| 实参捕获 | 提供 `captureAny`、`captureThat` 等调用匹配入口 | 捕获只在验证成功时提交；stub 和等待使用普通 matcher，不接受捕获模式 |
| 缺少 stub | Mockito 可生成缺失 stub 抛错或提供缺省响应的 mock；Mocktail 默认 null 可能触发返回类型错误 | 所有真实调用都要求匹配 stub，包括 void 方法；缺少时明确失败 |

这是一套借鉴 Dart 的 Vela API，不承诺与 Mockito／Mocktail 的全部语法和边缘行为相同。首版采用显式缺失调用失败，不提供 nice mock、零值响应或自动执行真实实现的模式。`called(0)` 按本节零次验证契约工作，不复制 Dart 框架已有的相关限制。

### 12.3 支持面与类型边界

生成器覆盖导出的非泛型接口及其普通方法，支持任意现有固定参数数量、命名实参调用、可空类型、显式实例化的泛型类、透明类型别名、容器、函数值、普通返回值、void 和 `Future<T>`。生成对象完整实现所选接口，不能只生成当时被测试引用的方法。

语言已支持接口类型参数和泛型方法，见[泛型契约](generics.md)；接口继承与接口属性仍不在当前语言范围内。当前 Mock 生成器面向非泛型接口及其普通方法，不提供开放泛型接口或泛型方法的完整适配。普通方法的参数和返回类型可以引用如 `Box<int>` 的合法具体类型。

普通类、构造函数、静态方法、顶层函数、扩展方法和内置对象不接受运行时替换。已有依赖应通过接口或函数值传入。回调依赖另提供 `MockFunction0<R>`、`MockFunction1<A, R>`、`MockFunction2<A, B, R>`，以及 `MockVoidFunction0`、`MockVoidFunction1<A>`、`MockVoidFunction2<A, B>`；泛型实参由完整的目标类型标注提供，或在构造时显式书写，仅凭 MockSession 实参无法推导，R 不允许为 void。这些类型暴露可传给业务代码的 `function`、创建调用模式的 `query(...)` 和 `control`，复用同一记录与验证核心。更多参数的函数可通过生成接口适配，不引入隐含的语言参数数量上限。

生成的真实接口方法保留原参数及返回类型。调用描述方法保留参数名称与数量，将各参数类型显式设为 dynamic，返回 `MockCall`；参数绑定后按原声明顺序归一化。因此 `load(42)` 与 `load(id: 42)` 描述相同模式，真实调用采用哪种书写方式不影响匹配。不会通过搜索测试表达式、猜测函数名或反向解析内部特化名称来重建签名。

`thenReturn` 等配置接口通过方法描述中的真实返回契约检查动态输入，不能把生产接口改成 dynamic。错误返回类型在配置时拒绝；自定义响应的实际结果还要经过生成适配器的受查转换。void 有独立的调用分支，不能存储为 dynamic 或用 null 冒充 void。现有 `sdk:test` 的 matcher 原样复用，不再建立另一套深度相等规则。

### 12.4 会话、调用模式与记录

`MockSession` 是显式的测试范围对象，统一拥有序号分配、控制器、stub、记录、未完成 builder 和异步等待。每个测试或它的 `setUp` 创建新会话；构造时绑定真实测试执行上下文，不允许在注册期或 `setUpAll` 创建会话供后代共享。一个测试可以拥有多个会话。顺序验证只能组合来自同一会话的模式。

配置、实际调用和验证必须属于创建会话的测试范围。其它测试使用旧控制器属于当前调用方的使用错误，不能通过切换上下文把访问合法化；原测试产生的迟到异步调用仍按既有错误封套归属原测试。会话和 mock 身份使用会话内数值 ID，不使用名称或宿主地址作为唯一键。

`MockCall` 是不可变的调用模式，包含所属控制器、方法身份及按参数顺序保存的匹配条件。构造模式不记录真实调用，不选择 stub，不运行 matcher，不执行响应，也不推进 Future。模式绑定原控制器，不因出现同名 mock 而改变目标；会话关闭后不能再使用。

每次真实调用在匹配 stub 和执行响应之前登记一条记录，并取得会话内严格递增的序号。记录包含 mock 身份、方法身份、规范实参、调用位置和验证状态。匹配失败、响应抛错及响应返回错误类型的调用仍保留记录。Future 的完成不额外产生第二次方法调用。

传给 `thenAnswer`／`thenDo` 的 `MockInvocation` 提供只读的 mock 身份、方法身份、方法名、序号、注册与调用位置，以及 `argumentAt(index)`、`argument(name)` 和 `arguments`。实参只保存一份有序存储，按名称访问复用声明生产者给出的名称索引；`arguments` 返回浅拷贝，不能借此改写内部记录。

实参对象保留原引用，不隐式深拷贝。之后修改数组或对象时，后续验证观察当前对象内容；需要匹配调用时的某个字段，可在响应中显式提取并保存。`same(value)` 验证身份，`equals(value)` 遵循测试库的深度匹配。框架不得调用业务 getter、`toJson` 或用户转换方法来保存或显示记录。

### 12.5 Stub 配置与响应

| API | 契约 |
| --- | --- |
| `when(call: MockCall)` | 返回尚未提交的 `MockStub`，不消耗真实调用 |
| `thenReturn(value)` | 返回非 Future、非 void 的固定值；重复调用返回同一值或对象引用 |
| `thenReturnInOrder(values)` | 配置非空的有限值序列；按匹配该 stub 的调用次数依次取值，耗尽即失败 |
| `thenThrow(error)` | 同步抛出配置的原始异常实例，异常必须符合现有 throw 契约 |
| `thenAnswer(callback)` | 接收一个 `MockInvocation`，计算普通返回值或返回原签名要求的 Future |
| `thenDo(callback)` | 专用于 void 方法，接收一个 `MockInvocation`，以语句形式调用返回 void 的同步回调 |

每个 builder 必须且只能提交一种响应；完成配置的动作才发布 stub。空序列、非法异常、非 callable、错误回调形参或不适合该方法的响应种类属于 `MockUsageError`。未终结的 builder 在会话收尾时报错，不能使一条看似已配置的规则被静默忽略。

响应优先级只取决于配置顺序，不推测哪条规则“更具体”。匹配阶段对候选 stub 从新到旧求值；匹配到第一项后执行一次响应。有限序列耗尽不重新选择较早的规则，也不重复最后一个值。需要交错返回与抛错时，使用具有显式局部状态的 `thenAnswer`。

回调签名在配置边界使用既有 callable 元数据校验，支持普通闭包和合法绑定方法；不能通过试调用检查签名。响应和 matcher 在普通脚本帧中执行，遵循共享原生调用边界。自定义响应调用其它 mock 或再次调用同一 mock 时，按普通调用栈处理并取得后续序号。

无匹配规则时产生 `MockMissingStub`，诊断包含实际调用、该方法的候选配置及首个有意义的不匹配。不会返回 null、构造任意业务实例或调用真实服务。该行为也适用于 void 方法，允许测试通过显式 `thenDo` 接受预期的无返回调用。

### 12.6 实参 matcher 与捕获

调用模式中的普通期望值包装为 `equals(value)`；实现 `Matcher` 的对象按 matcher 使用。可直接使用 `same`、数值比较、组合 matcher 和 `predicate`。另提供 `mock.any`，匹配包括 null 在内的任意单个参数。把 matcher 对象本身作为业务期望值时，显式使用 `same(matcher)`。

匹配按参数声明顺序进行，并在首个不匹配处停止；一次操作中，同一模式与同一候选调用之间的每个参数 matcher 最多求值一次。诊断复用首次匹配结果，不为生成说明再次执行 predicate 或响应。matcher 必须同步。异步 matcher 在此边界拒绝；自定义 matcher 抛出的异常原样传播，不能转成“未匹配”继续寻找其它 stub。匹配期间不允许在同一会话触发真实 mock 调用或改变配置、历史与验证状态，避免重入改变当前候选集；响应执行期间不受这一限制。

验证提供 `captureAny` 和 `captureThat(matcher)`，分别捕获任意实参和满足 matcher 的实参。例如：

```vela
const result = mock.verify(
  repository.calls.save(mock.captureAny, mock.captureThat(isNotEmpty))
).called(2);
expect(result.captured, equals([1, 'Alice', 2, 'Bob']));
```

示例假设此前依次调用 `save(1, 'Alice')` 和 `save(2, 'Bob')`，并已配置允许这些调用的 stub。捕获结果按调用序号、再按参数声明顺序展平；同一参数是数组时保留该数组对象，不再展开。结果同时提供匹配调用的只读描述，以便需要按调用分组时使用。

只有完整匹配且验证成功提交后才发布捕获结果；匹配失败、次数失败和顺序验证回滚均不泄漏部分捕获。捕获模式用于验证，不用于 `when` 或 `untilCalled`；错误使用在创建相关操作时拒绝。由此捕获结果不受 stub 搜索重试和等待观察影响。

### 12.7 次数验证、顺序与消费规则

| API | 契约 |
| --- | --- |
| `verify(call).called(count)` | count 为非负 int 时要求精确次数；为同步 Matcher 时匹配 int 计数 |
| `verifyNever(call)` | 等价于对未验证记录执行零次验证 |
| `verifyInOrder(calls)` | 按会话序号匹配不同的未验证记录，全部成功后整体标为已验证 |
| `verifyZeroInteractions(control)` | 要求当前历史中没有任何真实调用，包括已经验证的调用 |
| `verifyNoMoreInteractions(control)` | 要求没有尚未验证的真实调用 |
| `clearInteractions(control)` | 清空该 mock 的调用历史，保留 stub 及其序列当前位置 |
| `reset(control)` | 清空该 mock 的 stub、历史和序列状态，并使其未提交操作失效 |

`verify(call)` 只选择创建操作时已有的未验证记录，形成快照；`called` 成功后才标记这些记录并返回捕获结果。创建快照之后新增的调用不进入本次验证。忽略 builder 不算验证成功；重复终结同一 builder、对已被其它验证消费的记录提交旧 builder，或在历史已被清空后提交旧 builder，都产生使用错误。失败不消费记录，因此可以在捕获失败后检查相同历史。

`called(0)` 与 `verifyNever` 成功时都不消费记录。传入计数 matcher 时仍复用 `sdk:test` 的实际匹配结果；失败保留计数与 matcher 差异。没有负数或浮点次数的隐式转换。

`verifyInOrder` 为每个模式选择上一个序号之后最早的匹配未验证记录，一项匹配一次真实调用。允许忽略模式之间的其它调用；那些记录保持未验证，可由 `verifyNoMoreInteractions` 发现。相邻相同模式也必须由不同调用满足；整个序列失败时，所有选中状态和捕获都回滚。空序列属于使用错误，不产生一次没有意义的验证成功。

序号在整个会话内共享，跨 mock 的顺序可直接验证；reset 和 clear 不重用旧序号。不同会话或不同测试的调用不能合并验证。验证针对方法调用发生顺序，不针对 Future 完成顺序。

### 12.8 Future 与等待调用

Future 方法使用 `thenAnswer` 返回符合原声明的 `Future<T>`，包括 `Future<void>`。每次真实调用都执行响应回调，在该调用的测试上下文中创建或取得 Future。`thenReturn(Future...)` 和 `thenReturnInOrder` 中的 Future 在配置边界拒绝，防止把在配置范围预先失败的 Future 带入另一执行范围。

框架不自动 await 响应，不把所有响应包装成 async，不更换响应的原始异常对象。`thenThrow` 对 Future 方法仍是同步抛出；需要 Future 失败时，使用会返回失败 Future 的 `thenAnswer`。调用验证本身不观察或处理该 Future 的错误，错误仍由用户 await、matcher 和既有未处理错误检查点负责。

`untilCalled(call)` 返回 `Future<MockInvocation>`：若当前历史已有匹配调用，包括已验证的调用，选择最早一项并完成；否则等待之后首个匹配调用。登记等待前检查历史与发布观察必须处于同一执行线程的不可重入边界，避免丢失其间发生的调用。一次调用可以唤醒多个观察者；等待不会消费验证状态。

等待在真实调用记录完成后触发，不等待响应返回或 Future 完成。`untilCalled` 复用测试驱动的异步期望登记，调用者没有 await 时也保留在所属测试范围内，并受原测试的无活动超时、取消和迟到错误归属约束。匹配调用完成等待时更新心跳，无关调用、重复查询和日志不更新心跳。未发生调用时不会提前报假 AsyncIdle。

等待的 matcher 在检查历史或发布新记录时同步求值；抛错使对应等待 Future 以原异常失败，保留等待登记的上下文和源码位置，不中止被观察调用的正常 stub 响应。Future 的监听器沿既有微任务调度执行，不能在发布记录的同步分派中重入验证或响应。

`clearInteractions` 不撤销已登记的未来调用等待。`reset` 使该 mock 的等待以 `MockReset` 失败；显式关闭会话使尚未完成的等待以 `MockSessionClosed` 失败，均保留登记时的测试归属。取消与超时不撤销已经执行的响应，不承诺中断同步代码或宿主调用。

### 12.9 生命周期、GC 与失败报告

会话可以在 `setUp`、测试体及该测试的清理阶段使用。框架在该测试所有动态清理和静态 `tearDown` 完成后、发布最终执行阶段结束事件前，结束会话；不能直接用在 `setUp` 中登记的普通 `addTearDown` 提前关闭会话，导致后面的 teardown 无法验证 mock。

接入 suite 的内部范围收尾边界，按普通原生回调路径完成未提交 builder 检查、等待关闭和句柄释放。这个内部登记需要补全 SDK producer、运行时桥接和 suite 消费者；不能依赖 C++ 析构期间调用脚本，也不新增公开生命周期钩子。自动收尾失败标记为 `tearDown` 阶段，发布 `testDone` 前推进其已就绪的完成通知与错误检查点。显式 `session.close()` 使用同一收尾契约，重复关闭无额外效果；关闭后的新配置、调用与验证明确失败。

stub 闭包、异常、期望值、调用实参、捕获结果和挂起等待均使用现有精确根与写屏障，不能仅保存在未追踪的宿主容器中。模式或验证结果被用户保留时，只保留其真实需要的脚本值；会话关闭释放内部注册、未完成等待和响应，不能让每个历史条目永久保根整个 Program。不同 suite／Isolate 不共享控制器、序号或脚本值。

使用错误和不满足预期的交互沿现有 `TestFailure` 通道报告，分类区分 `MockUsageError`、`MockMissingStub`、`MockStubExhausted`、`MockVerificationFailure`、`MockReset` 与 `MockSessionClosed`。错误类别由真实 SDK 声明身份和结构化数据提供，不能从用户消息文字推断。自定义响应或 matcher 抛出的业务异常保留原对象与第一次失败栈，不能再包装成一个 mock 失败。匹配抛错不替代实际调用记录。

验证失败至少显示期望调用、次数或顺序、实际匹配数量、相关未验证调用，以及配置／验证位置和真实调用位置。复用现有结构化差异、原始源码栈和有预算的值显示，不从终端文本反向解析机器事件。断言与交互失败使用退出码 1，不阻止独立 suite 继续执行。

统一错误事件增加可选 `mock` 对象，包含 `mockSessionID`、`mockID`、规范方法名、期望说明、实际次数和有预算的相关调用列表；调用项保存序号、显示用实参和真实源码栈。worker 发布复制后的文本与数值，不跨 Isolate 传递脚本值。沿用 `vela.test` 版本 1 的已有字段与语义，消费者须忽略未知可选字段；删改已有字段、必填性或解释规则时必须提升协议版本。所有 reporter 消费同一事件，不另建第二条 stdout 日志流。

### 12.10 生成器、SDK 身份与工具链

命令如下，所有路径均相对调用时工作目录解析：

```text
vela mock API_FILE --interface NAME [--interface OTHER] -o OUTPUT
vela mock API_FILE --interface NAME -o OUTPUT --check
```

生成只处理显式选中的、由 API_FILE 导出的接口；至少提供一个 `--interface`。选择基于解析与绑定后的声明身份，不按源码文本搜索。重复选择同一声明去重，输出按规范模块身份与声明位置稳定排序。API_FILE 是独立的 API 模块，生成过程不运行 main、全局初始化器、测试注册或业务构造函数，也不扫描未声明目录。

生成器复用生产模块图、导出／私有性检查、类型别名解析、callable 签名和格式化。检查未通过或签名包含无法在输出模块合法命名的私有类型时，报告具体声明与原因，不能擦除为 dynamic、忽略成员或生成注释占位实现。可以通过公开的类型别名合法命名的类型应保留该公开名称。

每个包装对象生成完整的真实实现、调用描述入口和控制器连接。内部辅助标识按实际声明集合分配不冲突的名字；方法身份来自规范接口与成员声明，不能依赖临时 AST 地址、导入前缀或名称字符串猜测。零参数、同名接口的不同导入身份、可空与函数类型、任意固定参数数量及 void 分支均属于必须接通的生产路径。

生成产物是普通 `.vela` 模块，使用明确的文件导入与 `sdk:mock` 导入。目标输出必须是可编译的 Vela 源文件，不能覆盖 API 输入或其依赖。正常生成先完成全部语义检查、格式化和产物类型检查，再原子替换单个输出文件；失败保留旧文件，内容未改变时不重写。生成头记录输入声明指纹、生成器版本和 SDK 描述契约版本，不记录时间戳或机器私有路径。

`--check` 生成同一预期文本并与输出比较，不写文件；完全相同返回 0，缺失或过期返回 1，参数错误 64、源码或接口契约错误 65、内部失败 70、I/O 失败 74。正常编译、LSP 和测试运行不隐式重新生成文件；CI 应在编译前运行 `--check`。未运行生成命令时，缺失生成模块按现有模块加载错误报告。

`sdk:mock` 与 `sdk:test` 使用同一嵌入资源生产管线、声明绑定和不可覆盖的 SDK 身份。复用后者的 `Matcher`、差异和失败类型，不复制其源码或建立 stub 声明。SDK 内部导入也必须沿生产 SDK 模块图加载。生成器不引入 Python 专用 Vela parser、额外 mock 配置文件或项目级隐式生成扫描。

`check`、磁盘／内存编译、原生映像、缓存和 LSP 消费同一套 SDK 与生成源码。缓存使用真实 API 依赖、生成源和 SDK 的现有内容指纹；配置了不同 stub 或验证条件属于运行时状态，不加入编译缓存键。生成器模板版本参与生成检查，改变 SDK 宿主契约时进入既有依赖指纹。

LSP 补全、悬停、命名参数、跳转和文档读取真实生成声明；真实对象展示业务参数类型，调用描述入口明确展示接收值／matcher 的参数和说明。生成文件中的辅助符号导航到真实生成源码，SDK 符号保持只读 SDK URI。原接口位置作为来源元数据用于诊断关联，不能把不存在的文件或行号当作跳转结果。

### 12.11 数据结构与性能契约

SDK 核心拥有匹配、响应和验证状态；生成代码拥有接口类型适配；runtime 只提供必要的真实签名、上下文和收尾桥接。普通方法调用继续走现有原生调用路径，不在语言的所有成员访问上安装 mock 分派分支。

记录按控制器和方法身份建立索引；一次真实调用不扫描其它 mock 的历史。stub 查找限定于当前方法，最坏成本为该方法规则数乘实参匹配成本；次数验证扫描对应方法的候选记录，顺序验证复用会话序号与有序索引。不得为每个 mock 复制同一份会话序号表或完整模块元数据。

自定义 matcher 的成本与副作用由实际求值决定，不按显示预算跳过匹配。保留验证需要的历史，不能为限制内存静默丢弃旧调用。通过 `clearInteractions`、`reset` 和测试范围关闭显式释放；大规模调用的历史与根保留量须纳入测量。调用栈记录拥有已解析的函数名、源码路径、位置和版本身份，不保留机器码所有者；脚本数组展开和文本格式化留到诊断消费时完成。

Release 测量区分生成／检查时间、生成产物编译与缓存命中、单次已配置调用、不同规则数、次数与顺序验证、参数捕获、异步等待，以及大量记录后的内存回收。使用同签名的手写接口实现作为调用成本基线，记录匹配器实际比较次数；另测未使用 mock 的普通方法与 Future 路径。按已有滚动摘要规则保存聚合证据，不以代码行数或单个微基准取代完成契约。

### 12.12 实施阶段与验收

| 阶段 | 必须完成的生产路径 |
| --- | --- |
| M0：契约与生成模型 | 固定公开 API、调用描述／真实实现的职责和全部当前接口类型边界；诊断身份与序号模型确定 |
| M1：SDK 核心与同步调用 | 登记 `sdk:mock`，接通记录、stub 优先级、所有响应种类、匹配、缺失调用失败和函数替身；真实 void 契约闭环 |
| M2：验证与捕获 | 次数 matcher、零次、顺序、跨 mock 顺序、捕获、验证消费／回滚、清理和 reset 全部接通 |
| M3：接口生成器 | 生产语义图到完整接口对象／调用描述／控制器，CLI、原子输出、`--check`、生成代码类型检查与稳定输出完成 |
| M4：异步与工具集成 | Future 归属、untilCalled、超时／取消、测试范围收尾、GC、统一报告、SDK 导航、缓存和镜像身份完成 |
| M5：交付验证 | Debug/Release 回归、真实 CLI／LSP／生成检查、Release 性能与资源测量完成，文档状态与实际支持一致 |

M0–M5 是这项扩展的完整规格。只完成同步核心、只提供手写适配、缺少生成器或异步收尾时，都不能将“相对完整的 mock”标为交付完成。

行为测试至少覆盖最后规则优先、重复调用与序列耗尽、null 与错误类型、任意自定义异常原对象、所有响应分支、响应重入、matcher 抛错、循环／共享实参、无副作用的有预算显示，以及用户明确写出的独立预期结果。

验证测试覆盖 `.called(0)`、次数 matcher、失败不消费、成功后排除、过期 builder、捕获顺序与回滚、同一模式出现多次、顺序中间有无关调用、不同 mock 共用序号、禁止跨会话验证、clear／reset 和未终结配置。不能只断言实现生成的调用文本符合自身规则。

生成测试覆盖全部当前接口类型与调用形态、参数改名、接口增删方法、公开别名与不可命名私有类型、辅助名称冲突、稳定排序、输出过期、缺失输出、输出与输入冲突、失败不破坏旧产物，以及编辑器查询不执行生成或脚本。真实进程验证生成后的代码经 `check`、`run`、`vela test`、镜像和内容缓存进入同一生产路径。

异步和资源测试覆盖登记前／后已发生调用、多等待者、已验证历史、Future 未完成／失败／采用、响应跨 await、等待不消费调用、无关调用不延长超时、清理阶段验证、超时后的迟到调用、reset／close 期间等待，以及旧测试对象不能成为下一测试的 mock。GC 覆盖响应、实参、异常、捕获与挂起等待；suite 关闭后验证根、挂起帧、timer 和宿主端点回收。

M0–M5 的生产实现见 [SDK](../../compiler/sdk/mock.vela)、
[生成器](../../compiler/tooling/mock/mock_generator.cc)、
[原生桥接](../../runtime/native/native_mock.cc)和[结构化报告](../../compiler/testing/mock_report.cc)。
运行与测试命令使用同一 JIT/原生契约。源码检查、缓存命中、并发 suite、异步收尾和
GC 回收都必须在迁移后的 Debug/Release 路径完成验收。

Release 基准分别测量生成、检查、原生编译、目标缓存、调用、规则规模、验证、捕获、
等待与资源回收，并保留不导入 mock 的普通方法和 Future 对照。
每项校验独立预期结果与 matcher 求值次数，完整 GC 后保留量回到预热基线，挂起帧为零。
堆计数不是进程 RSS 或历史峰值，按调用数摊销的批次成本也不是孤立原生指令成本。
旧解释执行数据只作历史记录；当前中位数、波动和边界按[基准说明](../../benchmarks/README.md)
及[滚动摘要](../../benchmarks/results/rolling_summary.json)记录。

## 13. 参考

- [Dart package:test 使用与运行语义](https://github.com/dart-lang/test/blob/master/pkgs/test/README.md)。
- [Dart 测试配置：paths、filename、timeout 与 concurrency](https://github.com/dart-lang/test/blob/master/pkgs/test/doc/configuration.md)。
- [Dart 注册与生命周期 API 的真实签名](https://github.com/dart-lang/test/blob/master/pkgs/test_core/lib/src/scaffolding.dart)。
- [Dart Invoker：异步错误、心跳与超时](https://github.com/dart-lang/test/blob/master/pkgs/test_api/lib/src/backend/invoker.dart)。
- [Dart VM suite 加载：Isolate 与不同编译模式](https://github.com/dart-lang/test/blob/master/pkgs/test_core/lib/src/runner/vm/platform.dart)。
- [Dart JSON reporter 协议](https://github.com/dart-lang/test/blob/master/pkgs/test/doc/json_reporter.md)。
- [Mockito：配置、匹配、捕获、验证、异步响应与 nice／classic mock](https://github.com/dart-lang/mockito/blob/master/README.md)。
- [Mockito null safety：生成接口与录制返回值的实际约束](https://github.com/dart-lang/mockito/blob/master/NULL_SAFETY_README.md)。
- [Mocktail：闭包 API、参数占位与验证消费](https://github.com/felangel/mocktail/blob/main/packages/mocktail/README.md)。
- [Mocktail 实现：noSuchMethod、录制状态与交互验证](https://github.com/felangel/mocktail/blob/main/packages/mocktail/lib/src/mocktail.dart)。
- Vela [模块](modules.md)、[异步](async_tasks.md)、[异常](exceptions.md)、[动态值](json.md)与[源码工具链](source_tooling.md)。
