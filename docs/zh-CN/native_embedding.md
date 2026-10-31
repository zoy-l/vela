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

参数支持 `std::int64_t`、`double`、`bool`、`std::string_view`、
`Future` 和 `CancellationToken`。返回值还支持 `void`、拥有存储的 `std::string`、
可空的 `std::optional<std::string>` 以及 `std::expected<T, NativeError>`。
精确标量结果使用直接调用，托管或可失败结果经过受查 codec。
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

`ExecutionResult::value` 借用堆，包括错误载荷。下一次分配或 GC 前，使用
`RootedValue(isolate, value)` 保留托管结果。副本共享一个不可变根槽，最后一个句柄释放它；
读取 `value()` 不增加根。Isolate 必须晚于所属实例、执行、保根值及其他句柄销毁。

异步结果通过 `RootFutureValue` 转为保根的 Future 句柄，宿主推进所属调度器。
需要等待外部生产者时使用 `WaitForHost` 模式。
命令行应用通过 `ProcessEntry` 应用退出码规则并排空后台任务；嵌入宿主自行管理事件循环。

脚本可调用值保留其原生程序状态。调用必须属于相同 Isolate，并具有兼容代码身份或相同
可重载分派域。对可调用值保根即可使其所需代码保持可达。
除明确提供跨线程完成或安全点端点的 API 外，宿主操作必须在 Isolate 所属线程执行。

替换 Isolate 前先释放外部借用。`ReloadBlockers()` 返回根区间、保根值、原生实例、
句柄、入口、候选发布、Future、活动执行及正在进行的宿主访问。
存在阻塞资源时，`TryStop()` 保留当前运行；成功时关闭完成端点、丢弃排队工作并回收不可达状态，
Isolate 仍由所有者销毁。`FreezeBindings()` 禁止后续注册，并为新 Isolate 提供不可变绑定快照。
运行时内部根不免除宿主的保根责任。

## 诊断

原生栈帧使用实际执行版本的源码位置。调用方指向调用指令，异步错误保留抛出与 await 边界。
错误结果拥有解析后的函数名、路径和文本，可以晚于代码退休继续存活；
成功执行不捕获调用轨迹。

编译与运行时诊断使用 `path:line:column: error: message`，运行时错误追加
`at function (path:line:column)`。缺少源码元数据时报告指令位置。
该边界不使用 C++ 异常或非局部跳转，而是显式返回失败并保持宿主 RAII。
另见[调试器](debugger.md)与[执行契约](native_execution.md)。
