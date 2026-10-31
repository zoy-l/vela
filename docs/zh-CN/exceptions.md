# 异常处理

[English](../en/exceptions.md) | [**简体中文**](exceptions.md)

`throw` 语句接收一个非空的类（class）或接口（interface）实例。Catch 子句按源码声明顺序匹配具体的类类型或显式接口实现。基本类型值和数值永远不会隐式转换为错误对象。

```typescript
class ValidationError {
  message: string;
}

func validate(value: int): int {
  if (value < 0) { throw ValidationError(message: "negative value"); }
  return value;
}

func main(): int {
  try {
    return validate(-1);
  } catch (error: ValidationError) {
    print(error.message);
    return 42;
  } catch (error: Error) {
    print(error.message);
    return 1;
  } finally {
    print("finished");
  }
}
```

末尾的 `catch { ... }` 可以捕获任何异常，且不向局部暴露未类型化的值。若需绑定异常变量，则必须使用 `catch (name: Type)` 语法。重复的捕获类型以及在通用 catch 之后的 catch 子句均会被编译器拒绝。try 块必须跟随 catch、finally 或两者兼具。Catch 块绑定的变量遵循常规词法作用域及可变闭包捕获规则。

`catch (error: dynamic)` 同样捕获任何异常，并绑定原始异常实例，供通用异常匹配等代码使用。它必须位于其它 catch 子句之后；访问绑定值时仍遵循 `dynamic` 的运行时检查。

全局内置的 `Error(message: string, code: string)` 类无需导入即可使用，其中 `code` 默认为 `"Error"`。这是一个具有固定内存布局的普通类。运行时执行失败时会具体化为该类型：`message` 包含诊断信息，`code` 使用稳定的 `ExecutionStatusCode`，例如 `"DivisionByZero"` 或 `"NativeFailure"`。诊断文案与错误码分离，修改文案不能改变错误码。领域 API 在各自契约中定义命名错误码；显式构造的 Error 保留用户传入的 code。自定义错误类型无需继承任何通用基类。原生 C++ 失败与运行时受查失败走完全相同的 catch 处理路径。

异步完成错误中，第二次请求完成使用 `FutureAlreadyCompleted`，直接或间接采用自身使用 `FutureSelfAdoption`。`TimeoutException` 是独立的可抛出类，默认 code 为 `Timeout`。原生 `Future<T>` 完成的值类型不符或错误不可抛出时产生 `TypeError`。根 Future 没有可推进其执行的生产者时，宿主执行状态为 `AsyncIdle`；不会向挂起任务注入异常。传播与调度规则见[异步契约](async_tasks.md)。

Finally 块在正常完成、return、break、continue 以及异常传播时均会执行。return 表达式在进入 finally 前计算一次；在 finally 中修改局部变量无法覆盖先前计算得出的返回值。若 finally 内部执行了 return、break、continue 或 throw，则会直接取代待定的完成状态。finally 内部抛出的错误由外层区域处理，绝不会被当前同一个 try 的 catch 子句捕获。只有真正被 break/continue 退出的 finalizer 才会执行；跳出 try 内部的循环不会导致 try 提前退出。finally 在其自身的词法作用域中解析变量名，不受被保护代码体内名称遮蔽的影响。

编译器检查会在进入 catch 时保守丢弃非空细化信息，因为先前的任意操作都可能已经修改了被捕获的变量或局部状态。构造函数分析会在检查 return 初始化以及传播 break/continue 初始化状态之前执行 finalizer。catch/finally 的异常入口不能假定仅在 try 代码体内写入的字段已经完成初始化。

未捕获的执行结果会保留原始运行时状态（对于脚本显式 throw 则记录为 `Thrown`）以及带源码感知的位置调用栈。自动穿过 finally 向上冒泡会完整保留原始抛出异常的栈帧，即使 finally 中调用了其它函数或处理了另一个嵌套错误。显式的 `throw value` 会开启一个新的抛出点。失败的惰性全局初始化器仅重置实际展开的栈帧，以便捕获该错误的调用者能够重新尝试初始化。

[数值解析](numeric_parse.md)定义 `IntParseError`、`DoubleParseError` 和 `InvalidRadix`，[UTC 转换](time.md)定义 `DateParseError` 与 `DateRangeError`，均沿用相同的 Error/catch 机制。tryParse 仅将非法数字文本或 int64 溢出转为 null，非法进制仍会抛出错误。

## 表示法与性能

IR 指令记录最内层处理器，CFG 在活跃性分析和优化前纳入异常后继边。
原生 lowering 为失败生成通向对应 catch/finally 块的控制流边，并保留源码位置。
宿主边界不跨越 C++ 异常或非局部跳转。

隐藏 GC 包络为抛出实例保根，并保存状态与消息。单独拥有的诊断保存解析后的源码版本栈帧，
不借用机器码。catch 暴露带类型载荷；finally 执行期间，待处理包络保持为精确根。
替换待完成结果后，旧包络及其诊断可回收。堆计数包括包络和内联消息，
不包括所有宿主侧诊断分配。实际内存耗尽可能阻止错误包络创建，此时直接返回 OutOfMemory。
