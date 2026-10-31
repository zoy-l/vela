"""Generated and explicit class constructors preserve initialization contracts in AOT."""

SOURCE = """
external func collect(): void;
class Trace {
  static value: int = 0;
  static mark(value: int): int {
    Trace.value = Trace.value * 10 + value;
    collect();
    return value;
  }
}
class Record {
  value: int;
  text: string?;
  step: int = Trace.mark(8);
  values: int[] = [];
  private hidden: int = Trace.mark(3);
  constructor { Trace.mark(4); }
}
class Explicit<T> {
  value: T;
  text: string = 'default';
  private delta: int;
  constructor(initial: T) { this.value = initial; this.delta = 2; }
  adjustment(): int { return this.delta; }
}
class Base { value: int; constructor { this.value++; } }
class Child extends Base {
  extra: int;
  constructor { super(value: this.extra * 20 - 1); }
}
class Optional { text: string? = null; }
class GenericBase<T> { stored: T; read(): T { return this.stored; } }
class GenericChild<T> extends GenericBase<T> {
  constructor(value: T) { super(stored: value); }
  override read(): T { return super.read(); }
  get value(): T { return this.stored; }
  set value(next: T): void { this.stored = next; }
}
func identity<T>(value: T): T { return value; }
func callbacks<K, V>(values: Map<K, V>): (() => int)[] {
  const result: (() => int)[] = [];
  for (const [key, value] in identity(values)) {
    result.push((): int => key.length + value);
  }
  return result;
}
class LoopInitialized {
  value: int;
  constructor(values: int[]) {
    for (const value in values) {
      try { if (value == 0) continue; break; } finally { collect(); }
    }
    this.value = 42;
  }
}
class LocalPoint { x: int; y: int; text: string?; }
class SavedPoint { static point: LocalPoint? = null; }
class Link { value: int; next: Link?; other: Link?; }
func borrowWrite(point: LocalPoint, fail: bool): int {
  const alias = point;
  alias.x += 1;
  alias.text = 'new' + ' reference';
  collect();
  if (fail) { throw InitFailure(code: 1); }
  return point.x;
}
func borrowedFields(fail: bool): int {
  const point = LocalPoint(x: 27, y: 1, text: null);
  try { borrowWrite(point, fail); }
  catch (error: InitFailure) { point.y += error.code; }
  finally { collect(); }
  return point.x + (point.text ?? '').length + (fail ? point.y - 1 : point.y);
}
func borrowThenPublish(): int {
  const point = LocalPoint(x: 27, y: 1, text: null);
  borrowWrite(point, false);
  publishPoint(point);
  collect();
  return (SavedPoint.point == point ? point.x : 0) + (point.text ?? '').length;
}
func heapAndVirtualIdentity(): int {
  const first = LocalPoint(x: 14, y: 0, text: null);
  let previous = LocalPoint(x: 0, y: 0, text: null);
  let sum = 0;
  for (let index = 0; index < 3; index++) {
    const current = LocalPoint(x: index, y: 0, text: null);
    sum += previous.y;
    sum += first != current ? first.x : 0;
    previous = current;
  }
  return sum;
}
func borrowIncrement(point: LocalPoint): void { point.x += 1; }
export func borrowedRecursion(depth: int): int {
  const point = LocalPoint(x: 27, y: 1, text: null);
  borrowIncrement(point);
  return depth == 0 ? point.x : borrowedRecursion(depth - 1) + point.y;
}
func cyclicGraph(): Link {
  const first = Link(value: 20, next: null, other: null);
  const second = Link(value: 22, next: first, other: first);
  first.next = second;
  first.other = second;
  return first;
}
export func createGraph(): Link { return cyclicGraph(); }
export func createLink(): Link { return Link(value: 42, next: null, other: null); }
func graphIdentity(): int {
  const first = cyclicGraph();
  collect();
  const second = first.next as Link;
  return first.other == second && second.next == first && second.other == first
             ? first.value + second.value : 0;
}
func publishPoint(point: LocalPoint): void { SavedPoint.point = point; point.x += 1; }
func localFields(choose: bool): int {
  const point = LocalPoint(x: 1, y: 2, text: 'a' + 'bc');
  const alias = point;
  if (choose) { alias.x = 36; } else { point.x = 10; }
  for (let index = 0; index < 3; index++) { point.y += index; }
  collect();
  const text = alias.text ?? '';
  return point.x + alias.y + text.length - 2;
}
func conditionalEscape(publish: bool): int {
  const point = LocalPoint(x: 40, y: 0, text: null);
  const alias = point;
  if (publish) { publishPoint(point); }
  else { alias.x += 1; }
  alias.y += 1;
  collect();
  return point.x + alias.y;
}
func returnedPoint(): LocalPoint {
  const point = LocalPoint(x: 20, y: 22, text: null);
  point.x += point.y;
  return point;
}
func failingField(): int { collect(); throw InitFailure(code: 2); }
func exceptionalFields(): int {
  const point = LocalPoint(x: 40, y: 2, text: null);
  try { point.x = failingField(); }
  catch (error: InitFailure) { collect(); return point.x + error.code; }
  return 0;
}
export func calculate(operation: int): int {
  Trace.value = 0;
  if (operation == 0) {
    const record = Record(step: Trace.mark(2), value: Trace.mark(1), text: null);
    return Trace.value == 2134 && record.value == 1 && record.step == 2 &&
           record.text == null && record.values.length == 0 ? 42 : 0;
  }
  if (operation == 1) {
    const first = Record(value: 40, text: null);
    const second = Record(value: 40, text: 'retained');
    first.values.push(42);
    collect();
    return Trace.value == 834834 && first.values[0] == 42 && second.values.length == 0 &&
           second.text == 'retained' && Optional().text == null ? 42 : 0;
  }
  if (operation == 2) {
    const box = Explicit(initial: 40);
    collect();
    return box.text == 'default' ? box.value + box.adjustment() : 0;
  }
  if (operation == 3) {
    const child = Child(extra: 2);
    collect();
    return child.value + child.extra;
  }
  if (operation == 5) {
    const values = Map<string, int>(); values.set('ab', 38);
    const getters = callbacks(values);
    const child = GenericChild(value: 0);
    collect();
    child.value = getters[0]();
    return child.read() + 2;
  }
  if (operation == 6) { return LoopInitialized([0, 1]).value; }
  if (operation == 7) {
    const values = Map<int, int>(); values.set(1, 9); values.set(2, 30);
    let total = 0;
    for (const [key, value] in values) {
      values.clear(); values.set(100, 100); collect();
      total += key + value;
    }
    const items = Set<int>(); items.add(1); items.add(41);
    let sum = 0;
    for (const item in items) {
      items.clear(); items.add(100); collect();
      sum += item;
    }
    return total == sum ? total : 0;
  }
  if (operation == 8) {
    const wide = Wide(value63: 10, value64: 10, value127: 10, value128: 12);
    collect();
    return wide.value63 + wide.value64 + wide.value127 + wide.value128;
  }
  if (operation == 9) {
    try { Failing(); }
    catch (error: InitFailure) { collect(); return error.code; }
    return 0;
  }
  if (operation == 10) { return localFields(true); }
  if (operation == 11) { return conditionalEscape(false); }
  if (operation == 12) { return conditionalEscape(true); }
  if (operation == 13) { return returnedPoint().x; }
  if (operation == 14) { return exceptionalFields(); }
  if (operation == 15) { return graphIdentity(); }
  if (operation == 16) { return borrowedFields(false); }
  if (operation == 17) { return borrowedFields(true); }
  if (operation == 18) { return borrowThenPublish(); }
  if (operation == 19) { return heapAndVirtualIdentity(); }
  const box = Explicit<string>(initial: 'retained');
  collect();
  return box.value == 'retained' && box.adjustment() == 2 ? 42 : 0;
}
"""

SOURCE += "\nclass Wide {\n" + "".join(
    f"  value{index}: int = 0;\n" for index in range(130)) + "}\n"

SOURCE += """
class InitFailure { code: int; }
func failDefault(): int { collect(); throw InitFailure(code: 42); }
class Failing { value: int = failDefault(); }
"""

HOST_TESTS = r'''
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

extern "C" const NativeImage* CLASS_IMAGE() noexcept;
using NativeClassTest = CollectingImageTest<CLASS_IMAGE>;

TEST_F(NativeClassTest, ConstructorsPreserveArgumentsDefaultsPrivateStateGenericsAndSuperUnderGc) {
  NativeExecution execution(*instance);
  ExpectIntegerOperations(CLASS_ENTRY, execution,
                          {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19});
}

TEST_F(NativeClassTest, FailedGraphMaterializationDoesNotPublishPartialAliasesOrKeepRoots) {
  const auto functions = CLASS_IMAGE()->functions.values();
  const auto graph = std::ranges::find_if(functions, [](const auto& function) {
    return function.name.text() == "createGraph";
  });
  const auto single = std::ranges::find_if(functions, [](const auto& function) {
    return function.name.text() == "createLink";
  });
  ASSERT_NE(graph, functions.end());
  ASSERT_NE(single, functions.end());
  NativeExecution execution(*instance);
  auto& heap = isolate.heap();
  heap.Collect();
  const auto before_sample = heap.allocated_bytes();
  NativeValue sample;
  ASSERT_EQ(Execute(single->entry, execution, {}, sample), ExecutionStatus::Returned);
  const auto allocation_bytes = heap.allocated_bytes() - before_sample;
  heap.Collect();
  const auto baseline = heap.object_count();
  for (const auto successful_nodes : {0U, 1U}) {
    heap.SetAllocationLimit(heap.allocated_bytes() + successful_nodes * allocation_bytes);
    NativeValue result = Integer(123);
    EXPECT_EQ(Execute(graph->entry, execution, {}, result), ExecutionStatus::OutOfMemory);
    EXPECT_EQ(result.kind, NativeValueKind::Integer);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 123);
    EXPECT_EQ(execution.frame_count(), 0U);
    static_cast<void>(execution.TakeException());
    heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
    heap.Collect();
    EXPECT_EQ(heap.object_count(), baseline);
  }
  NativeValue result;
  ASSERT_EQ(Execute(graph->entry, execution, {}, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.kind, NativeValueKind::Object);
}

void VerifyBorrowedRecursionOnSmallStack() {
  Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<Collect>("collect")));
  const auto instance = NativeInstance::Instantiate(isolate, *CLASS_IMAGE());
  ASSERT_TRUE(instance.has_value()) << instance.error().message;
  const auto functions = CLASS_IMAGE()->functions.values();
  const auto found = std::ranges::find_if(functions, [](const auto& function) {
    return function.name.text() == "borrowedRecursion";
  });
  ASSERT_NE(found, functions.end());
  NativeExecution execution(**instance);
  NativeValue result;
  const std::array shallow{Integer(14)};
  ASSERT_EQ(Execute(found->entry, execution, shallow, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42);
  const std::array deep{Integer(100000)};
  EXPECT_EQ(Execute(found->entry, execution, deep, result), ExecutionStatus::StackOverflow);
  EXPECT_EQ(execution.frame_count(), 0U);
  static_cast<void>(execution.TakeException());
  isolate.heap().Collect();
  ASSERT_EQ(Execute(found->entry, execution, shallow, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), 42);
}

#if defined(_WIN32)
DWORD WINAPI RunBorrowedRecursionOnSmallStack(void*) {
  VerifyBorrowedRecursionOnSmallStack();
  return 0;
}
#else
void* RunBorrowedRecursionOnSmallStack(void*) {
  VerifyBorrowedRecursionOnSmallStack();
  return nullptr;
}
#endif

TEST(NativeBorrowedStackTest, BorrowedRecursionPreservesStackFailureAndReleasesAllRoots) {
  constexpr std::size_t kStackBytes = std::size_t{128} * 1024;
#if defined(_WIN32)
  auto* thread = CreateThread(nullptr, kStackBytes, RunBorrowedRecursionOnSmallStack, nullptr,
                              STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  ASSERT_NE(thread, nullptr);
  const auto waited = WaitForSingleObject(thread, INFINITE);
  DWORD exit_code = 1;
  const auto read_exit = GetExitCodeThread(thread, &exit_code);
  EXPECT_NE(CloseHandle(thread), 0);
  ASSERT_EQ(waited, WAIT_OBJECT_0);
  ASSERT_NE(read_exit, 0);
  EXPECT_EQ(exit_code, 0U);
#else
  pthread_attr_t attributes;
  ASSERT_EQ(pthread_attr_init(&attributes), 0);
  const auto sized = pthread_attr_setstacksize(&attributes, kStackBytes);
  pthread_t thread;
  const auto created = sized == 0
                           ? pthread_create(&thread, &attributes, RunBorrowedRecursionOnSmallStack,
                                            nullptr)
                           : sized;
  EXPECT_EQ(pthread_attr_destroy(&attributes), 0);
  ASSERT_EQ(created, 0);
  ASSERT_EQ(pthread_join(thread, nullptr), 0);
#endif
}
'''
