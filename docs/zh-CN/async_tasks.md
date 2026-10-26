# 异步函数与 Future 语义

[English](../en/async_tasks.md) | [**简体中文**](async_tasks.md)

> 状态：已实现。本文是 async/Future 语言、运行时、宿主和 CLI 行为契约。

## 目标

Vela 保留熟悉的 `async` / `await` 语法，并以 `Future<T>` 表示异步结果。它采用 Future 风格的组合 API，不宣称等同于 JavaScript `Promise` 或 Dart `Future` 的全部细节。Future 是可多次观察、只完成一次的强类型值；完成错误沿用 Vela 的异常契约。

设计覆盖 async 函数、Future 组合、异常传播、异步 main、宿主异步操作、取消通知、timer、GC 生命周期和性能验证。async/await 是单线程协作式执行；它本身不创建并行工作。

## API 示例

```vela
class User { public param name: string; }

external func fetchUser(id: int): Future<User>;

async func loadUsers(ids: int[]): Future<User[]> {
    let first = fetchUser(ids[0]);
    let second = fetchUser(ids[1]);
    return await Future.wait([first, second]);
}

async func main(): Future<int> {
    try {
        let users = await loadUsers([7, 8]);
        print(users[0].name);
        return 0;
    } catch {
        print("Could not load users");
        return 1;
    }
}
```

## 语法与类型

- async 函数、方法和闭包以 `async` 标记；其显式返回类型为 `Future<T>`，调用表达式的类型也是 `Future<T>`。
- async 函数体可以 `return` 一个 `T`，或返回另一个 `Future<T>`；外层 Future 采用该 Future 的完成结果。普通函数也可以显式返回 `Future<T>`。
- 函数值统一使用实际调用结果表达类型，例如 `(int) => Future<User>`。`async` 不另行改变函数值类型，因此 async 函数与返回相同 Future 类型的普通函数可按同一函数契约调用。
- `await e` 只能出现在 async 函数中。若 `e` 是 `Future<T>`，结果为 `T`；若 `e` 是普通值，结果类型不变。可空 Future 的空值经 await 后仍为空。
- async lambda、实例方法、静态方法与接口实现遵循相同规则。构造函数、静态初始化器、模块初始化器不能是 async；模块顶层不允许 await。
- `Future<T>` 是可存储、传递、返回和捕获的内置泛型引用类型。Future 只采用另一个 Vela `Future`，不检查任意对象的 `then` 成员。

## Future API

```vela
const producer = Completer<int>();
const result = producer.future.then((value) => value + 2);
Timer.after(0, (): void => { producer.complete(40); });
const answer: int = await result;
```

示例在 async 函数内执行。`Completer<T>()` 也可从上下文推导 `Completer<T>`。其只读属性为 `future: Future<T>` 和 `isCompleted: bool`；提出完成请求时 `isCompleted` 即为 true，包括采用尚未完成的 Future。`complete(value)` 接受 `T` 或 `Future<T>`；`Completer<void>.complete()` 不接收实参。`completeError(error)` 要求非空的类或接口实例。重复完成抛出 `Error(code: 'FutureAlreadyCompleted')`，直接或间接采用自身抛出 `Error(code: 'FutureSelfAdoption')`。

`then` 从回调返回值推导结果类型；源为 `Future<void>` 时，成功回调不接收值参数。错误回调必须有一个非空的类或接口参数，未标注的 lambda 参数默认为 `Error`。回调只处理匹配该参数类型的异常，其余错误原样传播。`catchError` 必须返回源值类型或相同值类型的 Future。`whenComplete` 的无参 action 返回 `void` 或 `Future<void>`，异步 action 完成后才转发结果。可选回调可以为空。这些 API 使用静态回调类型，不引入 `dynamic` 或 `unknown`。

`Future<T>` 的结果类型支持协变，`Completer<T>` 保持不变性。工厂和回调 API 会采用返回的 Future，类型实参不允许嵌套的 Future 载荷。值任务的 `Future.wait` 返回 `Future<T[]>`；`Future<void>[]` 聚合为 `Future<void>`，因为 Vela 没有 `void[]` 值类型。后一种情况的 cleanUp 为无参回调，且不会被调用。工厂的显式类型实参和上下文结果类型都必须与载荷或计算结果匹配。

- `Future.value(value)` 和 `Future.error(error)` 分别创建成功或失败的已完成 Future。若 `Future.value` 接收另一个 Future，会采用其完成结果；不会解析任意对象的 `then` 成员。
- `Future.sync(computation)` 立即调用计算函数，将其返回值、Future 或抛出的异常转换为 Future 结果。
- `Future.microtask(computation)` 将计算函数加入微任务队列；`Future.delayed(milliseconds, computation)` 在 timer 到期后调用计算函数。`sleep(milliseconds)` 返回一个延迟完成为 `void` 的 Future。
- `then(onValue, onError)`, `catchError(onError)` 和 `whenComplete(action)` 返回新的 Future。回调可返回普通值或 Future；回调抛出的异常会使新 Future 失败。回调始终异步通知，即使源 Future 已经完成。
- `Future.wait<T>(values, eagerError: false, cleanUp: null)` 按输入顺序收集所有成功值；空输入立即成功并返回空数组。默认记录首个错误并观察到所有输入完成后再失败；`eagerError` 可让聚合结果在首个错误时立即失败。设置 `cleanUp` 时，聚合失败会清理已成功产生的非空值；其余输入仍会被观察。
- `Future.race<T>(values)` 采用第一个成功或失败结果；空输入保持未完成，未获胜的输入不会被取消。
- `future.timeout(milliseconds, onTimeout)` 在时限内采用源 Future；超时后采用 `onTimeout` 返回的值或 Future，否则以 `TimeoutException` 失败。超时不会自动取消源操作，源 Future 仍会被观察。
- `Completer<T>` 为事件回调和其它非 async 生产者创建并完成 Future。`complete` 可接收值或 Future，后者会被采用；`complete` / `completeError` 只能调用一次，错误值必须是 Vela 可抛出的对象。第二次完成是契约错误。完成通知始终异步执行，避免监听器重入生产者。

Future 可有多个 waiter。完成值或错误在所有监听器消费前保持有效。组合 API 会观察每个输入的完成结果，不会因聚合 Future 提前失败而丢弃后续错误。wait 失败后，每个成功产生的非空输入都清理一次，包括提前失败后才到达的成功结果。清理回调的错误按未处理 Future 错误报告，不替换聚合结果记录的首个错误。

wait 和 race 的输入共享一个 GC 槽对象来保存聚合引用。每个输入保留该对象，GC 标记聚合状态时只扫描一次引用列表；提前失败后，cleanUp 回调和后到的结果仍由共享槽保持可达。

`future.timeout(milliseconds, onTimeout: null)` 保持源值类型。`TimeoutException` 是独立的可抛出类，提供 `message: string` 和 `code: string`（默认为 `Timeout`）；使用对应类型的 catch 或错误回调处理。源 Future 自身产生的 `TimeoutException` 会原样传播，不触发 `onTimeout`。

## 执行与调度

调用 async 函数会立即执行函数体，直到返回、抛错或遇到首个 await，然后返回 Future。函数体中的异常转为失败 Future，不会同步抛给调用者。`await` 是异步边界：即使等待的 Future 已完成或表达式只是普通值，续延也会在当前 VM 执行片段退出后异步恢复。

调度顺序为：当前 VM 执行片段结束或挂起；按 FIFO 执行已排队的 Future 续延；微任务队列排空后处理一个 timer 或宿主事件；再排空该事件产生的微任务。一次 Future 完成按 waiter 注册顺序通知。

Future 有未完成、成功完成、失败完成三种可观察结果，不暴露可变状态。Future 本身不可取消。需要取消的宿主操作接收独立的 `CancellationToken`；取消请求由操作决定如何处理，Future 最终仍以正常值或 Vela 异常完成。

## 错误与生命周期

async 函数中的 `throw`、运行时错误和 await 传播错误使用 Vela 现有异常对象与源码追踪。若未在函数内部捕获，错误成为该函数 Future 的失败结果。await 失败 Future 会在续延位置重新抛出原异常，因此既有 catch/finally 规则保持一致。异步诊断保留原始抛出位置及 await 边界。

微任务检查点结束时，宿主会收到尚未处理的 Future 错误通知；稍后增加处理器时，宿主可收到相应的已处理通知。`main` 可以返回 `Future<int>`；其失败映射为程序运行失败。后台 Future 的未处理错误作为运行时诊断报告。

CLI 的 `run` 命令会识别零参数的 `async func main(): Future<int>`，启动根 Future，并驱动当前 Isolate 的微任务、timer 和宿主完成事件，直到根 Future 完成或没有任何可推进的工作。成功结果转换为进程退出码；失败结果打印保留源码位置和 await 栈的异步诊断。没有生产者的未完成 Future 不会单独保持进程存活，CLI 会报告 async idle/deadlock。

挂起的局部值、Future 结果、错误、等待者和排队回调在 GC 中保持可达。续延保留 Program 和诊断元数据，借用 Isolate；宿主必须让 Isolate 存活到其 Program、Future 句柄及原生生产者释放之后。VM 代码只在所属 Isolate 线程运行；外部线程投递完成消息，由该线程更新 Future。

Future 值、监听器、VM 帧、GC 状态和 timer 队列由 Isolate 线程独占，内部不使用互斥锁。宿主线程通过无锁 MPSC 消息队列发布不可变的完成消息，Isolate 每次应用一个宿主事件，再排空其微任务。等待宿主事件或 timer 时通过信号量休眠。完成请求的占位和取消状态使用原子操作。Future 状态读取、监听器注册、堆操作与 Program 销毁仍在所属线程执行。原生取消回调可能在取消或注册线程运行，VM 工作必须经调度器投递。

`NativeContext::CreateCompleter()` 登记一个存活的宿主生产者；完成操作或释放全部生产者句柄会注销事件源。`NativeContext` 将返回的原生 Future 绑定到自己的调度器，拒绝已属于其他 Isolate 的 Future。原生 Future 完成时检查 external 声明中的 `Future<T>` 载荷；值类型不符或错误不可抛出时，检查后的 Future 以 `TypeError` 失败。跨线程投递的堆值必须已属于该 Isolate，并在发布前保持保根；分配和编码在所属线程完成。

`FutureScheduler::PumpOne()` 和 `PumpUntil()` 在没有可执行工作或 timer 时返回 idle；`WaitForHost` 模式还会等待已登记的宿主事件源。挂起状态只保存活跃寄存器值，全局槽通过 Future 引用图追踪；不可达的未完成任务及 Future 结果循环仍可回收。REPL 的后续提交共享同一份可增长的全局存储，早先提交的续延能读取和更新当前全局值。

CLI 等待根 main Future 与所有仍存活的宿主事件源。单独一个没有生产者的未完成 Future 不保持进程存活；若根 Future 仍未完成且没有可推进它的工作，CLI 报告 async idle/deadlock，嵌入式 pump 则返回 idle，供宿主稍后继续。

## 宿主事件与 timer

异步结果语义不依赖时钟；I/O 与 timer 属于宿主事件层。标准库提供 `sleep(milliseconds)`、`Timer.after(milliseconds, callback)` 和 `Timer.periodic(milliseconds, callback)`。两个 Timer 工厂都返回 `Timer` 句柄，通过 `timer.cancel()` 取消；`Timer.after` 触发一次，`Timer.periodic` 定期触发。callback 在 Isolate 线程运行；返回的 Future 不会阻塞后续 tick，其错误按普通未处理 Future 错误规则报告。

所有 timer 使用单调时钟。interval 不补发过期 tick，避免积累无界回调队列；同一 deadline 的事件按创建顺序处理。每个宿主事件完成后，先排空由其触发的微任务，再处理下一个事件。

延迟是单调时钟可表示范围内的非负整数毫秒，周期 timer 的间隔必须为正数；非法时长抛出可捕获的 `Error`。Timer 回调无参数，返回 `void` 或 `Future<void>`。取消是幂等操作，在所属线程立即释放保留的回调。`CancellationToken()` 提供 `isCancelled: bool` 和幂等的 `cancel()`；原生类型编解码与宿主共享同一份取消请求状态。

## 性能与实现边界

语言契约不规定 VM 如何保存挂起执行状态，但要求普通同步函数保留现有快路径，async 调用不产生 await 时不进入挂起调度，挂起时只保留恢复执行所需的活动状态。恢复单个 Future 或 task 不得扫描整个 Program；GC 必须精确追踪跨挂起点存活的值。

async 语义贯通 AST、类型检查、Typed IR、优化、寄存器分配、字节码编码与校验、VM、调试器、宿主 API 和 CLI。await 可挂起且可抛错，是可观察操作；优化不能删除、跨越或错误重排。直接调用、闭包调用和接口分派都提供一致的 Future 结果。

必需回归覆盖：首个 await 前的同步副作用、无 await 的完成、已完成 Future 的异步续延、FIFO 通知、多 waiter、Future 采用、Future 组合的成功与失败、timeout、Completer、跨 await 的 catch/finally、所有调用形式、异步 main、未处理错误、挂起期间 GC、宿主线程完成、timer 顺序和 idle/deadlock 边界。

Release 基准分别测量无 await 的 async 调用、已完成 Future 的 await、挂起/恢复、并发 Future 数量、挂起状态的 GC 压力和大量 timer。报告同步 VM 回归、每个挂起执行保留的内存和首次异步分配成本。

## 实现参考

- Dart async/await 与 Future API：[语言指南](https://dart.dev/language/async)、[Future API](https://api.dart.dev/dart-async/Future-class.html)、[Future 实现](https://github.com/dart-lang/sdk/blob/main/sdk/lib/async/future_impl.dart)。
- QuickJS-NG 的 async frame、await 与 Promise job：[async 状态](https://github.com/quickjs-ng/quickjs/blob/master/quickjs.c#L21163-L21224)、[await 指令](https://github.com/quickjs-ng/quickjs/blob/master/quickjs.c#L20868-L20878)、[job 队列](https://github.com/quickjs-ng/quickjs/blob/master/quickjs.c#L2488-L2552)。
- QuickJS-NG 将 OS timer 放在宿主标准库：[timer](https://github.com/quickjs-ng/quickjs/blob/master/quickjs-libc.c#L2471-L2609)、[事件循环](https://github.com/quickjs-ng/quickjs/blob/master/quickjs-libc.c#L4852-L4876)。
- Luau coroutine 使用显式 `lua_State`、独立 VM 栈和手动 resume/yield，不提供 Future/Promise 调度：[线程状态](https://github.com/luau-lang/luau/blob/master/VM/src/lstate.h#L285-L305)、[resume/yield](https://github.com/luau-lang/luau/blob/master/VM/src/ldo.cpp#L814-L869)。
