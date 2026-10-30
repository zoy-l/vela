# 原生嵌入与运行时诊断

[English](../en/native_embedding.md) | [**简体中文**](native_embedding.md)

Vela 使用显式的虚拟机栈帧与可恢复的错误结果。C++ 异常、`longjmp` 以及隐式数值类型转换均不在这一边界之内。

## 强类型原生函数

在编译脚本前，先在宿主持有的 Isolate 实例上注册原生函数：

```cpp
std::int64_t Add(std::int64_t left, std::int64_t right) noexcept {
  return left + right;
}

vela::runtime::Isolate isolate;
auto registered = isolate.RegisterNative(
    vela::runtime::NativeFunction::Bind<Add>("add"));
```

脚本端显式声明原生绑定：

```typescript
external func add(left: int, right: int): int;

func main(): int {
  const operation: (int, int) => int = add;
  return operation(40, 2);
}
```

C++ 函数签名提供了完整的绑定契约。支持的原生参数类型包括 `std::int64_t`、`double`、`bool` 以及借用的 `std::string_view`。返回值类型还支持 `void` 和持有所有权的 `std::string`。原生函数可以返回 `std::expected<T, NativeError>` 以表达可恢复的宿主错误。原生函数必须声明为 `noexcept`；宿主算术越界与资源分配失败由宿主自身负责。绝不会为了适配签名而隐式静默转换 `int`、`float`、可空类型或形状不符的参数。

堆限额和 `OutOfMemory` 约束实际发生的存储申请。原生优化可以省略不可观测的实例存储，或在对象需要真实身份时才申请堆存储；宿主不能依赖每次脚本构造都增加堆计数，也不能依赖 VM 与原生后端在同一次构造时报内存不足。进入宿主 ABI 的对象始终具有普通堆身份，借用和保根规则不变。详见[逃逸分析与物化](escape_analysis.md)。

需要访问 Isolate、分配托管值或执行异步操作时，函数以 `NativeContext&` 作为第一个参数；普通 C++ 函数可以省略它，`Bind` 会推导出相同的脚本签名。AOT 的普通标量结果通过寄存器返回，标量 ABI 完全一致时直接调用原 C++ 函数指针；需要编解码或返回可失败结果时保留相应适配器。

显式可空的字符串返回值使用 `std::optional<std::string>`（或包含它的 expected），此时脚本声明必须返回 `string?`。其它类型不会为了满足这一契约而发生类型提升。可选的显式符号支持带命名空间的宿主注册，无需语言引入 namespace 语法：`external func readLabel(): string? = "app.readLabel";`。

`external func` 没有函数体，也不支持泛型参数。在模块限定前，其符号名就是声明的函数名。声明遵循常规模块可见性与别名规则；宿主注册表在单个 Isolate 范围内全局生效。编译过程将每个声明链接至其持有的不可变绑定，并诊断未注册符号及签名不匹配错误。常规的静态语法检查无需执行或注册宿主代码即可直接校验 external 声明。原生函数值支持间接函数调用。重复注册相同符号将直接报错，而非静默覆盖已有契约。

原生声明保存不可变绑定，不生成包装字节码。直接调用及解析后的间接调用借用调用者已保根的寄存器窗口，绑定适配器直接读取类型化载荷，不分配被调函数的寄存器文件，不经过 `Value` 参数中转缓冲，也不重复注册参数根。动态参数绑定使用独立的已保根参数窗口。轻量逻辑帧保留 external 声明的诊断和调试信息，宿主触发 GC 时也可观察调用参数。符号查找与实参数量验证在链接或加载边界完成；结构性字节码验证不能证明值类型，因此参数载荷仍由生成的绑定适配器检查。宿主返回的字符串会被验证为合法 UTF-8 并拷贝至 Isolate 堆中，非法文本直接报错。原生 Future 的成功载荷和错误值仍按声明验证，包括尚未完成的 Future。

底层 `NativeFunction::Invoke` 借用 `RegisterView`，调用者须提供与声明完全一致的实参数量，并在整个调用期间保持该窗口被保根。返回的引用须在后续分配前保根。同一 VM 的重入调用使用独立寄存器存储，保护外层调用的槽位与借用字符串，使其在嵌套执行和 GC 期间保持有效。

## 宿主保根与生命周期

`runtime::RootedValue(isolate, value)` 拥有一个稳定的堆保根槽位。对其复制会共享该不可变槽位；移动或重新安置句柄不会导致保根地址失效。最后一个句柄析构时释放该根槽位。为句柄赋新值通过常规 RAII 机制安全替换旧根。Isolate 的生命周期必须长于从属于它的每一个程序、函数句柄、根区间及 `RootedValue`。这些 API 共享 Isolate 的单线程所有权，不引入共享并发访问。

虚拟机返回结果中的裸 `Value` 是从堆中借用的。如果它包含堆引用，在后续进行任何内存分配或垃圾回收之前，必须先为其创建 `RootedValue`。读取 `handle.value()` 不会创建新的根。原生参数仅在函数调用期间保持被保根；持久的宿主引用必须创建其自身的 `RootedValue`。

替换 Isolate 前，宿主先停止创建新借用并释放外部句柄。`Isolate::ReloadBlockers()` 列出剩余根、原生实例、不透明原生句柄、命名声明入口、待发布的原生候选、Program、Future、活动执行及尚在进行的宿主释放操作；每项包含资源种类、该 Isolate 内的身份和引用计数。仍有资源时，`TryStop()` 返回相同的诊断，不改变当前运行。成功后关闭完成端点、丢弃排队回调与 timer，并回收不可达状态；它不销毁 Isolate，所有者仍需先释放运行时存储，再销毁 Isolate。运行时根使用 `RootOwner::Runtime`；普通宿主 `RootRange` 默认为外部根。内部 retained 句柄只用于运行时所有权，不能代替宿主释放自身借用。

`Isolate::FreezeBindings()` 保存固定的原生注册集合；之后的注册返回 `NativeRegistrationError::BindingsFrozen`。用该 `NativeBindings` 快照创建新 Isolate 时，共享同一份不可变的 binding 实现与签名，脚本状态独立创建。

脚本函数值引用其生产代码中的函数编号，调用时必须具有相同的代码身份；REPL 的连续提交共享这一身份。独立编译的 Program 无法调用通过原生 Future 载荷接收的其它 Program 的函数值，这类调用产生类型错误。保留堆中的函数值可以保留其类型身份；执行代码还需要保留生产它的 Program 或 FunctionHandle。

## 源码映射与错误诊断

每条编译后的字节码指令在经过优化与寄存器分配后，均完整保留其源文件标识、行号与列号。位置映射仅记录变化量，所有函数共享一张不可变的源文件路径表。位置检索采用二分查找，仅在调用栈追踪或开发者工具明确请求时才执行。字节码镜像加载时会对外部映射做快照并执行严格校验（包括排序、指令范围及源码标识）。不包含映射的字节码依然是合法的独立程序。

原生声明使用逻辑位置零表示源码映射及调试回调位置，不包含可执行字节码指令。

运行时失败会捕获所有活跃的显式栈帧，最内层栈帧排在最前。调用方的 PC 指针明确指向 call 调用指令本身，而非紧随其后的下一条指令。追踪文本持有其自身的字符串副本，因此即使已编译程序被销毁，诊断文本依然持久有效。编译器错误与运行时错误统一格式为 `path:line:column: error: message`；运行时错误追加 `at function (path:line:column)` 栈帧。不含调试符号的字节码则打印指令偏移量。成功执行的程序不分配任何追踪对象。

实现参考：Dart 的 external 函数声明模型（[语言规范](https://spec.dart.dev/DartLangSpecDraft.pdf)）、Luau 的显式栈帧行号检索（`VM/src/ldebug.cpp`）及其受保护执行机制（`VM/src/ldo.cpp`）。Vela 通过显式返回失败来保持 C++ RAII 栈展开机制的完整性，而非跨宿主栈帧进行非局部跳转。
