# 异常处理

[English](../en/exceptions.md) | [**简体中文**](exceptions.md)

`throw` 语句接收一个非空的类（class）或接口（interface）实例。Catch 子句按源码声明顺序匹配具体的类类型或显式接口实现。基本类型值和数值永远不会隐式转换为错误对象。

```typescript
class ValidationError {
  public param message: string;
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

全局内置的 `Error(message: string, code: string)` 类无需导入即可使用，其中 `code` 默认为 `"Error"`。这是一个具有固定内存布局的普通类。虚拟机执行失败时会具体化为该类型：`message` 包含诊断信息，而 `code` 包含 `ExecutionStatusName` 字符串（例如 `"division by zero"` 或 `"native function failed"`）。自定义错误类型无需继承任何通用基类。原生 C++ 失败与运行时受查失败走完全相同的 catch 处理路径。

Finally 块在正常完成、return、break、continue 以及异常传播时均会执行。return 表达式在进入 finally 前计算一次；在 finally 中修改局部变量无法覆盖先前计算得出的返回值。若 finally 内部执行了 return、break、continue 或 throw，则会直接取代待定的完成状态。finally 内部抛出的错误由外层区域处理，绝不会被当前同一个 try 的 catch 子句捕获。只有真正被 break/continue 退出的 finalizer 才会执行；跳出 try 内部的循环不会导致 try 提前退出。finally 在其自身的词法作用域中解析变量名，不受被保护代码体内名称遮蔽的影响。

编译器检查会在进入 catch 时保守丢弃非空细化信息，因为先前的任意操作都可能已经修改了被捕获的变量或局部状态。构造函数分析会在检查 return 初始化以及传播 break/continue 初始化状态之前执行 finalizer。catch/finally 的异常入口不能假定仅在 try 代码体内写入的字段已经完成初始化。

未捕获的执行结果会保留原始运行时状态（对于脚本显式 throw 则记录为 `Thrown`）以及带源码感知的位置调用栈。自动穿过 finally 向上冒泡会完整保留原始抛出异常的栈帧，即使 finally 中调用了其它函数或处理了另一个嵌套错误。显式的 `throw value` 会开启一个新的抛出点。失败的惰性全局初始化器仅重置实际展开的栈帧，以便捕获该错误的调用者能够重新尝试初始化。

## 表示法与性能

底层实现遵循 JVM 的[异常表与显式栈展开机制](https://docs.oracle.com/javase/specs/jvms/se25/html/jvms-2.html#jvms-2.10)。中间表示（IR）指令记录其所属的最内层异常处理器。控制流图（CFG）构建在活跃变量分析、死代码消除和寄存器分配之前就已经纳入了异常后继边。指令紧凑化过程会将异常处理器目标与普通跳转统一重映射。字节码编码生成已排序且互不重叠的受保护地址区间；字节码加载器会严密校验每个区间与 Catch 条目。虚拟机仅在发生错误时通过二分查找检索这些表。普通指令分派既无需压入/弹出处理器记录，也不注册待处理异常根。

一个隐藏的 GC 对象负责持有被抛出的实例、紧凑函数/PC 位置、执行状态与诊断文本。元数据与文本共享单次堆分配，计入堆内存分配压力。Catch 语句暴露出强类型的有效载荷对象。待处理的异常包装对象在 finally 执行期间作为普通精确 GC 根保持有效；嵌套异常无需全局异常栈、不泄漏宿主记录，亦不使用 `longjmp`。取代待处理状态只需让旧包装对象变为可回收状态即可。如果出现真正的内存耗尽无法创建异常封套，则直接向宿主返回 OutOfMemory。
