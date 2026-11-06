# 原生嵌入与运行时诊断

[English](../en/native_embedding.md) | [**简体中文**](native_embedding.md)

需要编译源码的宿主通过 `compiler::jit` 生成原生代码，再在自己的 Isolate 中创建
`runtime::NativeInstance`。AOT 宿主把生成代码链接到同一运行时。
源码构建链接 `//compiler:jit`；独立 AOT 宿主使用发行包的运行时库和生成的公共头文件。
运行时库本身不包含源码编译器。

## 编译与执行

下面的示例通过带类型的宿主绑定返回 `42`：

```cpp
#include <cstdint>
#include <expected>
#include <string>

#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_instance.h"

std::int64_t Add(std::int64_t left, std::int64_t right) noexcept {
  return left + right;
}

std::expected<std::int64_t, std::string> Run() {
  vela::runtime::Isolate isolate;
  if (!isolate.RegisterNative(vela::runtime::NativeFunction::Bind<Add>("add"))) {
    return std::unexpected("cannot register add");
  }
  auto compiler = vela::compiler::jit::Compiler::Create();
  if (!compiler) {
    return std::unexpected(vela::frontend::DiagnosticsText(compiler.error()));
  }
  const auto compiled = vela::compiler::jit::CompileSource(**compiler, R"(
external func add(left: int, right: int): int;
func main(): int { return add(40, 2); }
)");
  if (!compiled.Succeeded()) {
    return std::unexpected(vela::frontend::DiagnosticsText(compiled.diagnostics));
  }
  const auto instance =
      vela::runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  if (!instance) {
    return std::unexpected(instance.error().message);
  }
  const auto entry = (*instance)->FindFunction("main");
  if (!entry) {
    return std::unexpected("main is missing");
  }
  const auto result = vela::runtime::ExecuteNativeFunction(**instance, *entry);
  if (!result.Succeeded()) {
    return std::unexpected(result.message);
  }
  return result.value.AsInteger();
}
```

映像不依赖 AST 或编译器继续存活。其共享所有者保活已链接代码与元数据，
原生实例及保留的原生程序状态继续持有该所有者。不同实例拥有独立全局状态。
保留状态的更新使用 `DevelopmentSession` 和 `DevelopmentRepl`，
不直接替换封闭实例中的代码。详见[热重载](hot_reload.md)。

## 带类型的宿主绑定

在创建实例前注册绑定。`NativeFunction::Bind<Function>` 从 `noexcept` C++ 函数推导签名。
需要 Isolate、分配或异步操作的函数可将 `NativeContext&` 放在第一个参数；
该参数不出现在脚本签名中。

公共值 API 由 `<vela/host.h>` 提供。标量使用 `std::int64_t`、`double`、`bool`；
脚本引用使用 `Local<String>`、`Local<Array<T>>`、`Local<Function<R(Args...)>>`、
`Local<Future<T>>` 等类型化句柄。泛型实参使用逻辑类型，例如 `Array<String>`。
可空值使用对应边界值的 `std::optional`。字符串还支持 `std::string_view` 参数和
`std::string` 结果，分别显式借用和复制。可失败结果使用 `vela::Result<T>`，即
`std::expected<T, vela::Error>`。Persistent 只用于宿主存储，不能出现在绑定签名中。
只有整条适配路径满足直接调用契约时才使用直接入口；托管或可失败转换经过受查 codec。
不会隐式缩窄 int/float；宿主算术和资源失败由宿主负责。

external 声明没有函数体或泛型参数。显式符号可以指定注册表名称，例如
`external func label(): string? = "app.label";`。
实例化将声明与接收方 Isolate 的不可变绑定比对；缺失绑定、签名不匹配及重复注册均报错。
纯源码检查不要求注册，也不执行宿主函数。

生成的调用在宿主执行期间维持逻辑栈帧和精确 GC 根。借用字符串只在调用期间有效，
持久引用必须另行保根。返回字符串和错误消息检查 UTF-8。
Future 返回值在尚未完成时也持续保留声明的载荷检查。
重入脚本使用独立原生执行状态，保留外层调用的根。

分配消除可能移除不可观察的对象存储。堆上限作用于实际存储请求，因此不能用源码构造次数
推算堆分配次数。跨越宿主 ABI 的对象拥有堆身份，并遵循普通托管值的保根规则。

## 值、Future 与关闭

`HandleScope scope(isolate)` 拥有局部根，`Local<T>` 的所有副本在所属作用域退出时失效。
创建、读取和调用显式接收目标作用域，托管返回值在返回前已经保根。宿主导入内通过
`context.handles()` 取得本次调用的作用域。`local.Persist()` 返回
`Result<Persistent<T>>`，用于跨调用保存；副本共享一个根，最后一个所有者释放它。
`persistent.ToLocal(scope)` 在目标作用域建立独立局部根，随后可以重置 Persistent。
Isolate 必须晚于所属作用域、持久句柄、错误载荷、实例和执行资源销毁。

宿主调用公开脚本入口使用 `Entry<Signature>::Bind(instance, name)`，随后
`entry.Call(scope, args...)` 返回 `Result`。开发会话中的命名入口每次调用当前版本；
`entry.Capture()` 捕获具体版本。脚本交付的 `Local<Function<Signature>>` 使用
`callback.Call(scope, args...)`，保留闭包、接收者和代码版本。脚本异常通过 `Error`
保留原始载荷、诊断及栈帧，宿主原样传播时不丢失异常身份。

低层 `ExecutionResult::value` 仍借用堆，包括错误载荷。内部调用者在下一次分配或 GC 前
使用可失败的 `runtime::HostRoot::Create(isolate, value)` 保根；公共宿主优先使用上述
类型化入口，以完成成功值与异常载荷的统一交接。

公共 `Local<Future<T>>` 提供 `Read(scope)` 和 `Wait(scope, WaitMode::WaitForHost)`。
`CreateCompletion<T>(scope)` 返回 Future 与可跨线程保存的完成端点；普通 Local 和
Persistent 只能在 Isolate 所属线程操作。端点关闭后报告 Closed，不再保活 Isolate。
命令行应用通过 `ProcessEntry` 等待并显示根结果，完成后关闭本次运行的后台任务。
根 Future 未完成时持续等待完成或显式停止，即使没有生产者也不自动退出。
嵌入式普通调用返回类型化值，不自动打印、不退出宿主进程，也不清理其它任务。
宿主自行管理事件循环并选择是否等待返回的 Future。详见[程序入口与生命周期](program_lifecycle.md)。

脚本可调用值保留其原生程序状态。调用必须属于相同 Isolate，并具有兼容代码身份或相同
可重载分派域。对可调用值保根即可使其所需代码保持可达。
除明确提供跨线程完成或安全点端点的 API 外，宿主操作必须在 Isolate 所属线程执行。

替换 Isolate 前先释放外部借用。`ReloadBlockers()` 返回根区间、保根值、原生实例、
句柄、入口、候选发布、Future、活动执行及正在进行的宿主访问。
存在阻塞资源时，`TryStop()` 保留当前运行；成功时关闭完成端点、丢弃排队工作并回收不可达状态，
Isolate 仍由所有者销毁。`FreezeBindings()` 禁止后续注册，并为新 Isolate 提供不可变绑定快照。
运行时内部根不免除宿主的保根责任。

脚本公共类型和成员包装可通过
`vela host-header api.vela --namespace=app -o api.h` 生成；生成不执行模块初始化。
包含名义类型的 `Type<T>` 显式绑定实例或开发会话，内建类型可以绑定 Isolate。
AOT 生成头文件提供相同的逻辑类型与作用域参数，C 局部引用和持久句柄分别释放。
完整类型映射、成员操作与生命周期约定见 [C++ 互操作](cpp_interop.md)。

## 诊断

原生栈帧使用实际执行版本的源码位置。调用方指向调用指令，异步错误保留抛出与 await 边界。
错误结果拥有解析后的函数名、路径和文本，可以晚于代码退休继续存活；
成功执行不捕获调用轨迹。

编译与运行时诊断使用 `path:line:column: error: message`，运行时错误追加
`at function (path:line:column)`。缺少源码元数据时报告指令位置。
该边界不使用 C++ 异常或非局部跳转，而是显式返回失败并保持宿主 RAII。
另见[调试器](debugger.md)与[执行契约](native_execution.md)。
