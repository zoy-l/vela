"""Native Future composition fixtures linked with the standalone runtime."""

SOURCE = """
external func collect(): void;
external func fetch(): Future<int>;
external func record(value: int): void;
class Failure { public param message: string; }
enum Choice { first, second }
interface Worker { transform(value: int): Future<int>; }
class Task implements Worker {
  public async transform(value: int): Future<int> { await 0; collect(); return value + 2; }
}
class Box {
  public param value: int;
  public param choice: Choice = Choice.first;
  public get twice(): int { collect(); return this.value * 2; }
  public set twice(value: int): void { collect(); this.value = value / 2; }
  public add(first: int, second: int): int { collect(); return this.value + first + second; }
}
let effect = 0;
async func add(value: int): Future<int> { await 0; collect(); return value + 2; }
async func fifoInput(): Future<int> { await 0; return 42; }
async func repeatedFifoInput(order: Box): Future<Box> {
  await 0;
  await 0;
  Future.microtask((): void => {
    order.value = order.value * 10 + 1;
    Future.microtask((): void => { order.value = order.value * 10 + 3; });
  });
  return Box(value: 42);
}
async func finalAction(): Future<void> { await 0; collect(); effect = 2; }
func finalError(): void { throw Error(message: 'action', code: 'Action'); }
async func hostCallback(value: int): Future<int> { await fetch(); collect(); return value + 2; }
async func pendingCallback(value: int): Future<int> { await Completer<void>().future; return value; }
async func cleanup(): Future<int> {
  let sum = 0;
  const later = Completer<int>();
  const inputs: Future<int>[] = [Future.value(20), Future.error(Error(message: 'x')), later.future];
  const all = Future.wait(inputs, eagerError: true, cleanUp: (value): void => { collect(); sum += value; });
  try { await all; } catch {}
  later.complete(22);
  await later.future;
  return sum;
}
async func timerOrder(): Future<int> {
  let order = 0;
  Timer.after(0, (): void => { collect(); order = order * 10 + 2; });
  Future.microtask((): void => { collect(); order = order * 10 + 4; });
  await sleep(0);
  return order;
}
async func periodic(): Future<int> {
  const result = Completer<int>();
  let count = 0;
  const timer = Timer.periodic(1, (): void => {
    collect(); count += 1;
    if (count == 2) { result.complete(42); }
  });
  const value = await result.future;
  timer.cancel();
  return value;
}
export async func calculate(operation: int): Future<int> {
  if (operation == 0) { return await Future.value(40).then((value) => value + 2); }
  if (operation == 1) { return await Future.value(40).then(add); }
  if (operation == 2) {
    return await Future.error<int>(Failure(message: 'kept')).catchError(
      (error: Failure): int => { collect(); return error.message == 'kept' ? 42 : 0; });
  }
  if (operation == 3) {
    return await Future.error<int>(Error(message: 'x')).then((value) => value + 1, onError: (error) => 42);
  }
  if (operation == 4) {
    try { return await Future.error<int>(Failure(message: 'x')).catchError((error: Error) => 0); }
    catch (error: Failure) { return 42; }
  }
  if (operation == 5) {
    const value = await Future.value(40).whenComplete(finalAction);
    return value + effect;
  }
  if (operation == 6) {
    try { return await Future.value(0).whenComplete(finalError); }
    catch (error: Error) { return error.code == 'Action' ? 42 : 0; }
  }
  if (operation == 7) { return await Completer<int>().future.timeout(0, onTimeout: () => 42); }
  if (operation == 8) {
    try { return await Completer<int>().future.timeout(0); }
    catch (error: TimeoutException) { return 42; }
  }
  if (operation == 9) { return await Future.value(42).timeout(100000); }
  if (operation == 10) {
    const values = await Future.wait([Future.delayed(0, () => 40), Future.value(2)]);
    return values[0] + values[1];
  }
  if (operation == 11) { return await cleanup(); }
  if (operation == 12) { return await Future.race([Future.value(42), Future.value(0)]); }
  if (operation == 13) {
    const values = await Future.wait<int>([]);
    await Future.wait<void>([]);
    return 42 + values.length;
  }
  if (operation == 14) {
    const first = await Future.microtask<int>(() => Future.value(40));
    return first + await Future.delayed(0, () => 2);
  }
  if (operation == 15) { return await timerOrder(); }
  if (operation == 16) {
    let value = 42;
    const timer = Timer.after(0, (): void => { value = 0; });
    timer.cancel(); await sleep(0); return value;
  }
  if (operation == 17) { return await periodic(); }
  if (operation == 18) {
    const source = Future.value(Choice.second);
    const copied = await source.then((value: Choice): Choice => { collect(); return value; });
    const values = await Future.wait([source, Future.sync((): Choice => Choice.first)]);
    return copied == Choice.second && values[0] == Choice.second ? 42 : 0;
  }
  if (operation == 19) {
    const task: Worker = Task();
    return await Future.value(40).then(task.transform);
  }
  if (operation == 20) {
    const source = Future.value(40);
    const then: ((int) => int) => Future<int> = source.then;
    return await then((value) => value + 2);
  }
  if (operation == 21) {
    const source: dynamic = Future.value(40);
    const mapped = source.then((value: int): int => { collect(); return value + 2; });
    const value: int = await mapped;
    return (mapped is Future<int>) ? value : 0;
  }
  if (operation == 22) {
    const source: dynamic = Future.value(40);
    const preserved = source.whenComplete(finalAction);
    const ready = preserved.timeout(100000);
    const value: int = await ready;
    const pending: dynamic = Completer<int>().future;
    const timed = pending.timeout(0, onTimeout: (): int => 42);
    const other: int = await timed;
    return value + effect == other ? other : 0;
  }
  if (operation == 23) {
    const owner: dynamic = Completer<int>();
    const next: dynamic = owner.future;
    let caught = false;
    try { owner.complete('wrong'); } catch (error: Error) { caught = true; }
    owner.completeError(Error(message: 'bad'));
    const result = next.catchError((error: Error): int => 42);
    const value: int = await result;
    return caught && owner.isCompleted ? value : 0;
  }
  if (operation == 24) {
    const token: dynamic = CancellationToken();
    token.cancel();
    const timer: dynamic = Timer.after(100000, (): void => {});
    timer.cancel();
    return token.isCancelled ? 42 : 0;
  }
  if (operation == 25) {
    let value: dynamic = 20;
    value += 1;
    const values: dynamic = [value, 20];
    values[1] += 1;
    const text: dynamic = 'x42';
    const bytes: dynamic = ByteArray(length: 1);
    bytes[0] = 42;
    const floating: dynamic = 2.0;
    return text[1] == '4' && bytes[0] == 42 && -floating == -2.0 ? values[0] + values[1] : 0;
  }
  if (operation == 26) {
    const owner: dynamic = Completer<Choice>();
    const future: dynamic = owner.future;
    const mapped = future.then((value: Choice): int => value == Choice.second ? 42 : 0);
    const flag: dynamic = Choice.second;
    owner.complete(flag);
    const result: int = await mapped;
    const values: dynamic = [Choice.first];
    values[0] = flag;
    return values[0] is Choice && values[0] == flag ? result : 0;
  }
  if (operation == 27) {
    const box: dynamic = Box(value: 20);
    if (box.twice != 40 || box.add(second: 1, first: 21) != 42) { return 0; }
    const add = box.add;
    if (add(21, 1) != 42) { return 0; }
    box.twice = 42;
    box.choice = Choice.second;
    return box.value == 21 && box.choice == (Choice.second as dynamic) ? 42 : 0;
  }
  if (operation == 28) {
    const values: dynamic = [40, 2];
    values.push(7, 8);
    values.splice(2, 2);
    const copy = values.slice();
    copy.reverse();
    const index: int = copy[0];
    return values.length == 2 && index == 2 && copy[1] == 40 ? 42 : 0;
  }
  if (operation == 29) {
    const huge: dynamic = 9223372036854775807;
    const one: dynamic = 1;
    const zero: dynamic = 0;
    let caught = 0;
    try { huge + one; } catch (error: Error) { if (error.code == 'IntegerOverflow') { caught += 1; } }
    try { one / zero; } catch (error: Error) { if (error.code == 'DivisionByZero') { caught += 1; } }
    try { one + true; } catch (error: Error) { if (error.code == 'TypeError') { caught += 1; } }
    return caught == 3 && (one << 64) == one ? 42 : 0;
  }
  if (operation == 30) { return await Future.value(40).then(hostCallback); }
  if (operation == 31) { return await Completer<int>().future.then(add); }
  if (operation == 32) { return await Future.value(42).then(pendingCallback).whenComplete(finalAction); }
  if (operation == 33) { Future.microtask((): void => { collect(); record(42); }); return 0; }
  if (operation == 34) { return await Future.race<int>([]); }
  if (operation == 35) {
    const source: dynamic = [1, 2, 3];
    const mapped = source.map((value: int): int => { collect(); return value * 7; });
    const sum: int = mapped.reduce((total: int, value: int): int => { collect(); return total + value; }, 0);
    return (mapped is int[]) ? sum : 0;
  }
  if (operation == 36) {
    const values: dynamic = [21, 7, 14];
    values.sort((left: int, right: int): double => { collect(); return (left - right).toDouble(); });
    return values[0] + values[1] + values[2];
  }
  if (operation == 37) {
    const values: dynamic = [Choice.first, Choice.second];
    const mapped = values.map((value: dynamic): dynamic => { collect(); return value; });
    return mapped[0] == (Choice.first as dynamic) && mapped[1] == (Choice.second as dynamic) ? 42 : 0;
  }
  if (operation == 38) {
    const values: dynamic = [1, 2];
    try { values.forEach((value: int): void => { values.push(value); }); }
    catch (error: Error) { return error.code == 'ConcurrentModification' ? 42 : 0; }
    return 0;
  }
  if (operation == 39) {
    const values: dynamic = [1, 'bad'];
    let seen = 0;
    try { values.forEach((value: int): void => { seen += value; }); }
    catch (error: Error) { return error.code == 'InvalidCast' && seen == 1 ? 42 : 0; }
    return 0;
  }
  if (operation == 40) {
    const values: dynamic = [7, 14, 21];
    const some: bool = values.some((value: int): bool => value == 14);
    const every: bool = values.every((value: int): bool => value > 0);
    const found: int = values.findIndex((value: int): bool => value == 21);
    return some && every && found == 2 ? 42 : 0;
  }
  if (operation == 41) {
    const box = Box(value: 0);
    for (let index = 0; index < 6; index++) {
      box.value += await Future.value(7);
      collect();
    }
    return box.value;
  }
  if (operation == 42) {
    const text = 'saved:' + operation.toString();
    await 0;
    const values = [text, 'second'];
    await 0;
    collect();
    try { await Future.error<int>(Failure(message: values[0])); }
    catch (error: Failure) {
      await 0;
      collect();
      return error.message == 'saved:42' && values[1] == 'second' ? 42 : 0;
    }
  }
  if (operation == 43) {
    await 0;
    await 0;
    return Future.value(42);
  }
  if (operation == 44) {
    for (let index = 0; index < 64; index++) { await Future.value(index); }
    return 42;
  }
  if (operation == 45) {
    let order = 0;
    const input = fifoInput();
    Future.microtask((): void => {
      order = order * 10 + 1;
      Future.microtask((): void => { order = order * 10 + 3; });
    });
    const value = await input;
    order = order * 10 + 2;
    return value == 42 && order == 12 ? 42 : 0;
  }
  if (operation == 46) {
    const order = Box(value: 0);
    const result = await repeatedFifoInput(order);
    order.value = order.value * 10 + 2;
    return result.value == 42 && order.value == 12 ? 42 : 0;
  }
  if (operation == 47) {
    await 0;
    await Completer<void>().future;
    return 42;
  }
  return 0;
}
"""

HOST_TESTS = r'''
extern "C" std::uint8_t COMPOSITION_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" const NativeImage* COMPOSITION_IMAGE() noexcept;
std::int64_t composition_effects = 0;
void RecordComposition(NativeContext&, std::int64_t value) noexcept { composition_effects += value; }

void RegisterComposition(Isolate& isolate) {
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Collect>("collect")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Fetch>("fetch")));
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<RecordComposition>("record")));
}

std::string CompositionDiagnostic(const Future& future) {
  const auto diagnostic = future.error_diagnostic().value_or(FutureDiagnostic{});
  std::string text = diagnostic.message;
  for (const auto& frame : diagnostic.stack_trace) {
    text += "\n" + frame.function + ":" + std::to_string(frame.line) + ":" +
            std::to_string(frame.column) + " @" + std::to_string(frame.instruction);
  }
  return text;
}

TEST(NativeHostTest, FutureCompositionExecutesNativeCallbacksUnderGcAfterInstanceRelease) {
  for (std::int64_t operation = 0; operation <= 46; ++operation) {
    if (operation >= 30 && operation <= 34) { continue; }
    SCOPED_TRACE(operation);
    Isolate isolate(1);
    RegisterComposition(isolate);
    isolate.scheduler().SetUnhandledErrorHandler([](const Future& failed, Value) {
      ADD_FAILURE() << CompositionDiagnostic(failed);
    });
    Value value;
    RootRange root(isolate.heap(), &value, 1);
    {
      const auto instance = NativeInstance::Instantiate(isolate, *COMPOSITION_IMAGE());
      ASSERT_TRUE(instance.has_value()) << instance.error().message;
      NativeExecution execution(**instance);
      const std::array arguments{Integer(operation)};
      NativeValue output;
      ASSERT_EQ(Execute(COMPOSITION_ENTRY, execution, arguments, output), ExecutionStatus::Returned);
      value = DecodeNativeValue(output);
    }
    isolate.heap().Collect();
    auto* future = Heap::GetFuture(*value.AsObject());
    ASSERT_NE(future, nullptr);
    while (future->is_pending()) {
      ASSERT_TRUE(isolate.scheduler().PumpOne());
      isolate.heap().Collect();
    }
    ASSERT_EQ(future->state(), FutureState::Succeeded)
        << CompositionDiagnostic(*future);
    ASSERT_TRUE(future->value().has_value());
    EXPECT_EQ(future->value()->AsInteger(), 42);
    EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
    EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0U);
  }
}

TEST(NativeHostTest, RepeatedAwaitAllocatesOneResultFuturePerActivation) {
  Isolate isolate(std::numeric_limits<std::size_t>::max());
  RegisterComposition(isolate);
  const auto instance = NativeInstance::Instantiate(isolate, *COMPOSITION_IMAGE());
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const auto baseline = isolate.heap().object_count();
  const std::array arguments{Integer(44)};
  NativeValue output;
  ASSERT_EQ(Execute(COMPOSITION_ENTRY, execution, arguments, output), ExecutionStatus::Returned);
  auto future = Heap::GetFuture(*DecodeNativeValue(output).AsObject())->Rooted();
  ASSERT_TRUE(isolate.scheduler().PumpUntil(future));
  EXPECT_EQ(future.value()->AsInteger(), 42);
  // 64 个 awaited Future + 整次函数调用的一个结果 Future；没有恢复转接 Future。
  EXPECT_EQ(isolate.heap().object_count() - baseline, 65U);
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
  EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0U);
}

TEST(NativeHostTest, CompositionRetainsCallbackFutureAcrossHostCompletionAndGc) {
  Isolate isolate(1);
  RegisterComposition(isolate);
  Completer producer;
  ActiveProducer() = &producer;
  Value value;
  RootRange root(isolate.heap(), &value, 1);
  {
    const auto instance = NativeInstance::Instantiate(isolate, *COMPOSITION_IMAGE());
    ASSERT_TRUE(instance.has_value());
    NativeExecution execution(**instance);
    const std::array arguments{Integer(30)};
    NativeValue output;
    ASSERT_EQ(Execute(COMPOSITION_ENTRY, execution, arguments, output), ExecutionStatus::Returned);
    value = DecodeNativeValue(output);
  }
  ASSERT_TRUE(isolate.scheduler().PumpOne());
  ActiveProducer() = nullptr;
  isolate.heap().Collect();
  auto* future = Heap::GetFuture(*value.AsObject());
  ASSERT_NE(future, nullptr);
  EXPECT_FALSE(future->IsCompleted());
  std::atomic<bool> completed = false;
  std::thread worker([&] { completed.store(producer.Complete(Value::Integer(0)).has_value()); });
  const auto pumped = isolate.scheduler().PumpUntil(*future, FuturePumpMode::WaitForHost);
  worker.join();
  ASSERT_TRUE(pumped);
  EXPECT_TRUE(completed.load());
  ASSERT_EQ(future->state(), FutureState::Succeeded);
  EXPECT_EQ(future->value()->AsInteger(), 42);
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
}

TEST(NativeHostTest, UnreachableCompositionGraphsReleaseCallbacksAndSuspensions) {
  for (const auto operation : {31, 32, 34, 47}) {
    Isolate isolate(1);
    RegisterComposition(isolate);
    const auto baseline = isolate.heap().object_count();
    {
      const auto instance = NativeInstance::Instantiate(isolate, *COMPOSITION_IMAGE());
      ASSERT_TRUE(instance.has_value());
      NativeExecution execution(**instance);
      const std::array arguments{Integer(operation)};
      NativeValue output;
      ASSERT_EQ(Execute(COMPOSITION_ENTRY, execution, arguments, output), ExecutionStatus::Returned);
      if (operation == 32 || operation == 47) { ASSERT_TRUE(isolate.scheduler().PumpOne()); }
      EXPECT_GT(isolate.scheduler().suspended_frame_count(), 0U);
    }
    isolate.heap().Collect();
    EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U) << operation;
    EXPECT_EQ(isolate.scheduler().saved_state_bytes(), 0U) << operation;
    EXPECT_EQ(isolate.heap().object_count(), baseline) << operation;
  }
}

TEST(NativeHostTest, DiscardedComputationStillExecutesQueuedNativeSideEffects) {
  Isolate isolate(1);
  RegisterComposition(isolate);
  composition_effects = 0;
  const auto baseline = isolate.heap().object_count();
  {
    const auto instance = NativeInstance::Instantiate(isolate, *COMPOSITION_IMAGE());
    ASSERT_TRUE(instance.has_value());
    NativeExecution execution(**instance);
    const std::array arguments{Integer(33)};
    NativeValue output;
    ASSERT_EQ(Execute(COMPOSITION_ENTRY, execution, arguments, output), ExecutionStatus::Returned);
  }
  isolate.heap().Collect();
  ASSERT_TRUE(isolate.scheduler().PumpOne());
  EXPECT_EQ(composition_effects, 42);
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), baseline);
}
'''
