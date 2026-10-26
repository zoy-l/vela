# 正则表达式

[English](../en/regexp.md) | [**简体中文**](regexp.md)

> 状态：设计规格。RegExp 和 RegExpMatch 尚未实现。本文定义第一版 API、匹配语义、引擎集成与完成条件。

Vela 采用 Dart 风格的 `RegExp` 对象及匹配结果，底层使用独立 PCRE2 库。模式遵循固定 PCRE2 版本及下述配置的语法和匹配规则，位置沿用 Vela 的 Unicode 码点索引。构造时编译模式，匹配不修改 RegExp 对象上的游标。API 不承诺 ECMAScript 兼容性。

```vela
func main(): void {
  const pattern = RegExp('([A-Za-z]+)=([0-9]+)');
  const match: RegExpMatch? = pattern.firstMatch('age=42');
  if (match != null) {
    print(match.group(1) ?? ''); // age
    print(match.group(2) ?? ''); // 42
  }

  const numbers = RegExp('[0-9]+');
  const matches: RegExpMatch[] = numbers.allMatches('7, 42');
  print(matches.length); // 2
}
```

此 API 不依赖 dynamic 或 JSON。第一版不增加通用 Pattern/Iterable 协议、正则字面量、原始字符串语法、替换/分割 API 或匹配期间的用户回调。[字符串大小写转换](string_case.md)具有独立规格。

## RegExp API

| API | 结果／契约 |
| --- | --- |
| `RegExp(source: string, multiLine?: bool, caseSensitive?: bool, dotAll?: bool)` | 构造并编译模式，后三个参数默认分别为 false、true、false |
| `RegExp.escape(text: string): string` | 为 text 生成 PCRE2 字面匹配片段，不规定输出拼写 |
| `pattern.pattern: string` | 只读，传入的已解码模式字符串 |
| `pattern.isMultiLine: bool` | 只读 multiLine 配置 |
| `pattern.isCaseSensitive: bool` | 只读 caseSensitive 配置 |
| `pattern.isDotAll: bool` | 只读 dotAll 配置 |
| `pattern.isUnicode: bool` | 只读 true，始终使用 Unicode 模式 |
| `pattern.hasMatch(input: string): bool` | 是否存在匹配 |
| `pattern.firstMatch(input: string): RegExpMatch?` | 第一个匹配，无匹配时返回 null |
| `pattern.allMatches(input: string, start?: int): RegExpMatch[]` | 从 start 开始的全部非重叠匹配，start 默认为零 |
| `pattern.matchAsPrefix(input: string, start?: int): RegExpMatch?` | 在 start 处发起锚定匹配，start 默认为零，返回匹配或 null |
| `pattern.stringMatch(input: string): string?` | 第一个完整匹配的子串，无匹配返回 null |

RegExp 与 RegExpMatch 是专用 builtin 引用类型，不是用户子类或动态属性 Map。支持普通字段、参数、返回值、可空标注、捕获和不变的强类型容器。用户不能直接构造 RegExpMatch。所有属性只读。标准声明及依赖闭包沿用现有名称预留、局部遮蔽、overlay 与缓存指纹规则。

实参沿用 Vela 普通求值顺序与位置/命名调用规则。可选参数表示省略实参，不表示可传 null。不执行数字与字符串隐式转换。

## 匹配与 Unicode

PCRE2 定义模式语义，包括有序分支、贪婪/非贪婪及占有量词、捕获、反向引用、前后查找、Unicode 属性转义和内联选项。匹配使用 `pcre2_match`，不使用 DFA 接口，直接暴露原生回溯结果。不把模式翻译为另一种方言，也不维护 ECMAScript 语法检查器。沿用 PCRE2 自身约束，例如变长后向查找必须具有固定构建所允许的有限最大长度，不承诺支持后向查找中的无界重复。不支持的模式在编译时报错。

| 配置 | Vela 契约 |
| --- | --- |
| 代码单元宽度 | 只构建 8-bit 库，直接匹配 UTF-8 |
| `PCRE2_UTF` | 始终启用 |
| `PCRE2_UCP` | 始终启用，`\d`、`\w`、`\s` 等采用 PCRE2 Unicode 属性默认语义 |
| `PCRE2_NEVER_BACKSLASH_C` | 始终启用，禁止 `\C` 拆开 UTF-8 字符 |
| `multiLine` | 映射为 `PCRE2_MULTILINE`，初始为 false |
| `caseSensitive` | false 时启用 `PCRE2_CASELESS`，初始为 true |
| `dotAll` | 映射为 `PCRE2_DOTALL`，初始为 false |
| 换行约定 | 显式设置 `PCRE2_NEWLINE_ANY` |
| `\R` 约定 | 显式设置 `PCRE2_BSR_UNICODE` |
| JIT | 关闭，使用解释执行 |

构造属性报告初始选项。PCRE2 内联选项和所支持的模式指令保留原生含义，包括局部覆盖。UTF 模式与禁止 `\C` 的约束不能被放松。不启用 ECMAScript 兼容配置、locale 特定大小写配置，或允许在前后查找内使用 `\K` 的配置；尤其不能启用可能产生反向完整匹配范围的 `PCRE2_EXTRA_ALLOW_LOOKAROUND_BSK`。使用库的默认字符表，不从宿主 locale 生成字符表。

忽略大小写遵循 PCRE2 的 Unicode caseless 规则，不执行完整字符串大小写展开。不能先把输入转为小写或大写再匹配：这会改变位置，也不是引擎的比较算法。

`RegExp.escape` 转义 PCRE2 元字符，并安全处理字面空白与 `#`，使片段嵌入扩展模式时也不会改变含义。必要时使用标量十六进制转义。该方法不转义 Vela 源码字面量，也不将具体输出形式作为序列化契约。

Vela 字符串为合法 UTF-8，所有公开位置按 Unicode 标量值计数。强制 Unicode 模式，不提供 `unicode: false`。`allMatches` 立即返回现有 `RegExpMatch[]`，不增加惰性 Iterable。模式使用普通 Vela 字符串：`RegExp('\\d+')` 将反斜线和 d 交给模式编译器。第一版没有 Dart 原始字符串或 JavaScript 正则字面量语法。

匹配保留原始输入，不做规范化、字素簇分割或 locale 转换。`.` 在普通单字符匹配中消费一个码点，包括补充平面字符。模式和输入均支持 NUL，接口始终传递显式长度。组合形式与分解形式保持区别，除非模式显式匹配它们。Unicode 表来自固定版本的依赖数据，不使用宿主 locale 数据。

## 位置与多次匹配

输入长度为 N 时，start 必须满足 `0 <= start <= N`，非法值走现有越界错误路径。位置相对完整原始输入，匹配范围为半开区间 `[start, end)`。start 控制搜索位置，不改变输入原点或锚点与后向查找可见的上下文。引擎接收完整输入和字节起点。matchAsPrefix 对该次尝试使用 `PCRE2_ANCHORED`。原生 `\K` 可以把报告的完整匹配起点重置到锚定尝试开始之后，捕获范围也可以位于完整匹配范围之外；这些是 PCRE2 语义，不能因此报错或丢弃成功结果。

```vela
const match = RegExp('Vela').firstMatch('😀Vela');
if (match != null) {
  print(match.start); // 1
  print(match.end);   // 5
}
```

`allMatches` 从请求的位置搜索，非空匹配后从其 end 继续；空匹配后前进一个完整码点。在 N 处的空匹配只交付一次，随后结束。因此空模式产生 N + 1 个空匹配。量词或分支产生的空匹配遵循相同规则。

各次调用不共享迭代状态。同一对象和输入重复调用 firstMatch 或 hasMatch 得到相同结果。没有 lastIndex、global 或 sticky 标志；通过 matchAsPrefix 显式请求起点匹配。

## RegExpMatch API

| API | 结果／契约 |
| --- | --- |
| `match.start: int` | 完整匹配的起始码点位置 |
| `match.end: int` | 完整匹配的结束码点位置，不包含该位置 |
| `match.input: string` | 原始输入字符串 |
| `match.pattern: RegExp` | 产生该匹配的 RegExp |
| `match.groupCount: int` | 捕获组数量，不含第零组 |
| `match.groupNames: string[]` | 按 PCRE2 名称表顺序返回唯一名称快照，即 UTF-8 字典顺序 |
| `match.group(index: int): string?` | 零表示完整匹配，1..groupCount 表示捕获；未参与匹配的捕获返回 null |
| `match[index]: string?` | 与 group(index) 相同，只读 |
| `match.groups(indices: int[]): string?[]` | 按请求顺序返回捕获结果 |
| `match.namedGroup(name: string): string?` | 已定义的命名捕获未参与时返回 null，否则返回捕获字符串 |

参与匹配但长度为零的捕获返回空字符串，不是 null。第零组始终参与。保留原生 branch-reset 编号，groupCount 为最大合法捕获编号。模式启用重复名称时，namedGroup 沿用 PCRE2 选择首个参与捕获的规则；均未参与时返回 null。非法编号走现有越界错误路径，未知名称抛出 code 为 `RegExpGroupError` 的 Error。不支持负编号。`groups` 发布结果前验证编号并返回独立数组。每次 groupNames 访问返回独立快照，修改快照不影响已编译元数据。

## 引擎、所有权与失败

通过 DEPS 和 GN 集成 PCRE2，启用 Unicode，只构建 8-bit 源码闭包，关闭 JIT。其 C API 显式返回错误，适合 Vela 关闭 C++ 异常的构建。固定不可变 revision，在依赖记录中登记实际 BSD-3-Clause WITH PCRE2-exception 及随附数据许可证、Unicode 表版本、源码闭包与构建配置。不修改上游源码，也不保留另一套回退引擎。

通过 general context 接入 Vela 分配器，通过 compile context 设置上述配置，match data/context 由单次操作拥有。固定构建的 match、depth 和 heap 限制必须显式登记在 GN 配置与依赖记录中；它们属于实现上限，变化时需要回归审查。`PCRE2_ERROR_MATCHLIMIT`、`PCRE2_ERROR_DEPTHLIMIT`、`PCRE2_ERROR_HEAPLIMIT` 映射为独立的限制错误，不能解释为无匹配或实际分配失败。不设置无限预算；引擎计数不是墙钟 timeout。

PCRE2 接收原始 UTF-8 缓冲区和显式长度，返回字节偏移，不复制 UTF-16 输入。每次操作共享一份边界映射，将公开码点起点转为字节起点，并将捕获范围转换回码点位置。allMatches 只构建一次映射，不对每个捕获重复扫描前缀。已有内部 UTF-8 校验契约允许在重复匹配调用中使用 `PCRE2_NO_UTF_CHECK`，但不允许未验证的外部输入或码点内部起点。分配前检查大小和索引转换，不截断 `PCRE2_SIZE`。编译诊断中的字节偏移转换为已解码模式字符串的码点偏移。

RegExp 拥有编译程序、原始模式、配置和捕获名称元数据。RegExpMatch 强引用输入与 RegExp，拥有不可变捕获范围。结果不能指向单次操作拥有的 match data、工作缓冲区或借用的 arena。除公开坐标信息外，保存字节范围，使提取分组可直接复制选中的 UTF-8 内容，不扫描整个前缀。匹配对象可以比调用方的输入和模式变量存活更久。

GC 追踪输入/模式引用，发布引用使用已有屏障。编译程序和引擎工作缓冲区计入堆核算，由所有者在失败、回收或 Isolate 销毁时准确释放一次。分配安全点前保根接收者、输入、部分结果数组和新匹配对象。hasMatch 无须分配 RegExpMatch；仅在请求内容且无法复用时为分组提取分配字符串。

| 失败 | 结果 |
| --- | --- |
| 非法模式语法或不支持的模式约束 | code 为 `RegExpSyntaxError` 的 Error，消息含 PCRE2 诊断及已解码模式的码点偏移 |
| 非法搜索起点或捕获编号 | 现有 `IndexOutOfBounds` 路径 |
| 未知命名捕获 | code 为 `RegExpGroupError` 的 Error |
| 引擎/底层分配或可表示大小耗尽 | 现有可恢复 `OutOfMemory` 路径 |
| 匹配工作量、匹配深度或引擎堆限制耗尽 | code 为 `RegExpLimitError` 的 Error |
| 其它引擎执行失败 | 显式 `RegExpExecutionError`，不能当作无匹配 |
| 静态接收者、实参或编号类型错误 | 编译诊断；dynamic 边界保留运行时检查 |

无匹配是正常的 false、null 或空数组结果。引擎失败不能转为无匹配，allMatches 也不能返回部分数组。错误沿用普通 VM catch/finally 和源码调用栈，不能被替换为 JSON 或通用原生调用失败。捕获一次操作失败后，调用方仍可复用 VM 与 RegExp。

采用回溯引擎，不承诺通用的最坏线性复杂度。第一版不公开 timeout 参数。分配、已配置限制和引擎失败显式处理；性能验证覆盖歧义量词与失败搜索，不能只测成功的字面量匹配。

## 实现范围与完成条件

现有[标准环境](../../compiler/frontend/standard_environment.cc)、[builtin 签名](../../compiler/frontend/builtins.h)、[UTF-8 helper](../../base/utf8.h)和[运行时对象模型](../../runtime/object.h)界定集成边界。runtime 拥有编译模式与匹配结果，不依赖 compiler/tooling。普通字符串检索保持已有实现与语义。

| 阶段 | 必须完成的工作 |
| --- | --- |
| 依赖 | 通过 DEPS/GN 固定 PCRE2 8-bit Unicode 源码闭包、许可证、版本、配置与限制，关闭 JIT |
| 前端 | 增加专用 RegExp/RegExpMatch 类型、签名、构造、属性、匹配下标、可空结果、不变容器及可选/命名参数检查 |
| 运行时 | 接通编译/搜索、坐标转换、捕获、所有权、GC/核算和带类型错误映射 |
| IR 与 VM | 降低构造和全部调用，标记分配与可抛出效果，保留源码求值顺序和新结果的引用身份 |
| 镜像与缓存 | 记录类型/builtin 元数据并校验操作数，按需更新格式/构建身份；引擎编译程序与原生指针不写入镜像 |
| 工具链 | 补全、悬停、文档和格式化共享签名；调试器提供可用的模式/匹配表示，REPL 正确保留生命周期 |
| 交付 | 增加 `examples/regexp.vela`，全部 API 与验证路径完成后才勾选路线图 |

必要回归覆盖构造默认值及配置、Unicode 属性、有序分支、贪婪/非贪婪匹配、编号/命名捕获、未参与与空捕获、反向引用和前后查找、锚点与行终止符、Unicode 忽略大小写、补充平面字符及坐标转换、NUL、非法转义/模式、起点/捕获边界、空匹配与终止、重复调用独立性、分组快照和可恢复引擎失败。PCRE2 专项覆盖 Unicode 简写字符类、内联选项、换行指令、有界/无界后向查找、branch-reset 与重复名称、锚定匹配中的 `\K`、拒绝 `\C` 和前后查找内的 `\K`，以及各类已配置限制错误。兼容向量以固定 PCRE2 构建为准。GC 测试只保留匹配对象，施压 allMatches 分配与分组提取，并验证编译/工作缓冲区释放与核算。编译器、工具、新 Isolate 镜像和持久化 REPL 测试覆盖两种新类型。

实现完成边界执行相关 Debug/Release 回归、工具测试、静态检查和 Release benchmark。区分模式编译与重复匹配，测量 hasMatch 分配、allMatches 物化、坐标映射、捕获、内存压力和回溯案例。本文不声称已有实现或基准结果。

## 参考

- [Dart RegExp](https://api.dart.dev/dart-core/RegExp-class.html) 与 [RegExpMatch](https://api.dart.dev/dart-core/RegExpMatch-class.html)。
- [PCRE2 API](https://www.pcre.org/current/doc/html/pcre2api.html)与[模式语法](https://www.pcre.org/current/doc/html/pcre2pattern.html)，实现测试以固定版本为准。
- [PCRE2 源码](https://github.com/PCRE2Project/pcre2)与[许可证](https://github.com/PCRE2Project/pcre2/blob/main/LICENCE.md)。
- Vela [字符串](strings.md)、[异常](exceptions.md)与[垃圾回收](garbage_collection.md)。
