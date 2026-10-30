# 项目配置与测试框架设计

> 状态：T0–T5 已实现。本文定义通用项目配置 `vela.json`、面向 Vela 源码的测试库，以及 `vela test` 运行器。测试 API 和执行语义以 Dart `package:test` 为依据；配置复用已有 JSON 基础设施，测试路径必须显式提供。Debug/Release 回归、真实进程、静态检查与 Release 测量已在 Windows x64 完成；性能与资源证据见[滚动摘要](../../benchmarks/results/rolling_summary.json)。

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
| suite 执行 | 普通 VM 模式使用独立 Isolate，suite 之间并发 | 每个 suite 独立 Isolate，suite 内顺序执行；不固定为每文件一个进程 |
| 异步错误 | 通过 Zone 归属测试，迟到错误可能使已完成测试失败 | 接通内部测试上下文与相同的错误归属行为，不新增公开 Zone API |
| 超时 | 无活动超时，可按测试、分组和 suite 配置 | 保留；不将其解释成进程硬终止或任务取消 |

Dart 当前 `test` 与生命周期回调使用 `FutureOr<dynamic> Function()`，并非只接受 `void` 或 `Future<void>`。Vela 保留“普通返回值忽略、Future 等待完成”的行为，不为测试框架增加 `FutureOr`、联合类型、函数重载、泛型约束或类型实参推导。

第一版包括本机 VM 上的单元测试与集成测试、配置、发现、注册、生命周期、同步与异步断言、超时、标签筛选、并发及结构化报告。浏览器测试、跨平台选择表达式、注解语法、mock 生成、快照测试、覆盖率、分片、重试和随机顺序不在本次实现范围内；不宣称完整兼容 Dart 的全部 API 或配置。

### 1.2 可复用的生产实现

| 现有实现 | 本设计中的职责 |
| --- | --- |
| [`JsonDocument` 与 `JsonOutput`](../../base/json.h) | 读取通用项目配置，写入机器报告；不引入 YAML 解析依赖或第二套 JSON 解析器 |
| [模块加载器](../../compiler/frontend/source_loader.h) | 承载测试文件与 SDK 模块的同一依赖图、源码位置及缓存身份 |
| [标准环境](../../compiler/frontend/standard_environment.h) | 复用不可变 SDK 源码资源的构建和声明机制，区分内部 `builtin:` 来源与公开模块导入 |
| [函数调用签名](../../base/callable.h)与[运行时 callable 解析](../../runtime/callable.h) | 校验测试回调，保留闭包、绑定方法及所属程序的身份 |
| [`Future` 与 `FutureScheduler`](../../runtime/future.h) | 等待异步回调、推进微任务与 timer、观察未处理错误，传播测试上下文 |
| [CLI 入口](../../compiler/vela_main.cc)与[`ProcessEntry`](../../runtime/process_entry.h) | 集成新命令与异步驱动，避免两套彼此抢占的未处理错误处理器 |
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

`check`、`run`、`vela test`、字节码镜像、内存编译和 LSP 必须消费同一份 SDK 源码与声明身份。`sdk:test` 不能只在测试 CLI 中识别，也不能由语言服务再造一份 stub。内存编译仍只从明确提供的源码集合和内置 SDK 资源加载，不增加隐式磁盘读取。

### 4.2 同名 `test` 接受同步与异步

Vela 没有 `FutureOr`。测试库的注册边界使用现有 `dynamic` 保存回调，并根据 callable 的真实签名校验；不增加 `testAsync`，也不把所有回调强制声明成返回 `Future<void>`。

`test`、setup/teardown 和 `addTearDown` 接受可用零个实参调用的非空 callable。存在可选参数时使用已有默认实参规则，不能包含必须由调用方提供的参数。它可以返回 `void`、任意普通值或任意现有 `Future<T>`。普通值忽略，Future 的完成值忽略但必须等待成功或失败；同步 throw 与 Future 失败均进入相同的报告通道。泛型函数必须按现有语言规则先完成显式特化。

`group` 的注册回调必须为可用零个实参调用、返回 `void` 的同步 callable，执行后立即恢复外层注册作用域。不能让 async group 返回的 Future 被忽略，导致后续测试注册到错误分组。

真实回调签名由[既有调用元数据](../../base/callable.h)提供，不通过试调用推断。非函数、要求必填实参的函数、空值、错误的 group 返回类型都在注册边界报告注册失败。第一版不因静态参数为 dynamic 而承诺与 Dart 完全相同的编译期回调错误诊断；运行时校验是明确的公开边界。

框架回调通过已有 VM 调用和 Future 归一化路径调用，不能在 C++ builtin 中递归进入 VM，也不能把脚本 `void` 伪装成可存储的 dynamic 值。需要继续执行脚本的操作应返回到所属执行驱动，由普通脚本帧或既有续执行机制完成。

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

SDK 导出普通的 `Matcher` 接口和匹配结果类型，自定义 matcher 可以用 Vela 实现。同步接口为 `match(actual: dynamic): MatchResult`；异步接口 `AsyncMatcher` 为 `matchAsync(actual: dynamic): Future<MatchResult>`。`MatchResult` 保存成功状态、要求说明和可空的差异节点，差异节点保存路径、实际值说明及子差异，不将所有诊断提前拼接成单个字符串。`expect` 和 `expectLater` 根据实现的接口选择同步或异步流程；matcher 的源码调用通过普通脚本执行路径发生，不由 native helper 重入 VM。

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

可复用不可变的编译输入快照或已经验证可跨 Isolate 使用的现有编译产物；不能把在某个 Isolate 上实例化的闭包或全局存储直接交给另一个 worker。并行编译必须遵守现有编译缓存的发布与锁定契约。

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

`vela test` 第一版使用 VM。可重载 JIT、优化 JIT 和离线 AOT 不是用户可选的测试平台，本设计不增加不能兑现的后端参数；SDK 声明和运行时上下文不得依赖 VM 的私有 AST 地址，以便后续通过真实后端适配扩展。

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
| suite 驱动 | 编译器工具层连接编译、注册、VM callback 调用、Future 等待、生命周期和结构化结果 |
| 异步上下文 | runtime 持有与编译器无关的可选上下文与错误归属；VM 保存、传播和恢复它，不把 runner 配置塞入 runtime |
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

## 12. 参考

- [Dart package:test 使用与运行语义](https://github.com/dart-lang/test/blob/master/pkgs/test/README.md)。
- [Dart 测试配置：paths、filename、timeout 与 concurrency](https://github.com/dart-lang/test/blob/master/pkgs/test/doc/configuration.md)。
- [Dart 注册与生命周期 API 的真实签名](https://github.com/dart-lang/test/blob/master/pkgs/test_core/lib/src/scaffolding.dart)。
- [Dart Invoker：异步错误、心跳与超时](https://github.com/dart-lang/test/blob/master/pkgs/test_api/lib/src/backend/invoker.dart)。
- [Dart VM suite 加载：Isolate 与不同编译模式](https://github.com/dart-lang/test/blob/master/pkgs/test_core/lib/src/runner/vm/platform.dart)。
- [Dart JSON reporter 协议](https://github.com/dart-lang/test/blob/master/pkgs/test/doc/json_reporter.md)。
- Vela [模块](modules.md)、[异步](async_tasks.md)、[异常](exceptions.md)、[动态值](json.md)与[源码工具链](source_tooling.md)。
