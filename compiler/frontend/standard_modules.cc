#include "compiler/frontend/standard_modules.h"

#include <optional>
#include <string_view>

namespace vela::frontend {

auto StandardModuleSource(std::string_view name) noexcept -> std::optional<std::string_view> {
  if (name == kErrorModuleName) {
    return R"(export class Error {
  public param message: string;
  public param code: string = "Error";
}
)";
  }
  if (name == "std:console") {
    return R"(
export external func readLine(): string? = "std:console.readLine";
export external func write(text: string): void = "std:console.write";
export external func flush(): void = "std:console.flush";
)";
  }
  if (name == "std:time") {
    return R"(
export external func monotonicMicros(): int = "std:time.monotonicMicros";
export external func unixMicros(): int = "std:time.unixMicros";
)";
  }
  if (name == "std:random") {
    return R"(
export external func seed(value: int): void = "std:random.seed";
export external func nextInt(bound: int): int = "std:random.nextInt";
export external func nextDouble(): double = "std:random.nextDouble";
)";
  }
  if (name == "std:math") {
    return R"(
export const pi: double = 3.141592653589793;
export const e: double = 2.718281828459045;
export external func sqrt(value: double): double = "std:math.sqrt";
export external func log(value: double): double = "std:math.log";
export external func exp(value: double): double = "std:math.exp";
export external func sin(value: double): double = "std:math.sin";
export external func cos(value: double): double = "std:math.cos";
export external func tan(value: double): double = "std:math.tan";
export external func floor(value: double): double = "std:math.floor";
export external func ceil(value: double): double = "std:math.ceil";
export external func round(value: double): double = "std:math.round";
export external func trunc(value: double): double = "std:math.trunc";
export external func abs(value: double): double = "std:math.abs";
export external func pow(value: double, exponent: double): double = "std:math.pow";
export external func min(left: double, right: double): double = "std:math.min";
export external func max(left: double, right: double): double = "std:math.max";
export external func clamp(value: double, lower: double, upper: double): double = "std:math.clamp";
export external func isFinite(value: double): bool = "std:math.isFinite";
export external func isNaN(value: double): bool = "std:math.isNaN";
)";
  }
  if (name == "std:collections") {
    return R"(
func siftDown<T>(values: T[], root: int, length: int, compare: (T, T) => int): void {
  while (root < length / 2) {
    let child = root * 2 + 1;
    if (child + 1 < length && compare(values[child], values[child + 1]) < 0) { child = child + 1; }
    if (compare(values[root], values[child]) >= 0) { return; }
    const temporary = values[root]; values[root] = values[child]; values[child] = temporary;
    root = child;
  }
}
export func sort<T>(values: T[], compare: (T, T) => int): void {
  for (let root = values.length / 2; root > 0; root = root - 1) {
    siftDown<T>(values, root - 1, values.length, compare);
  }
  for (let end = values.length - 1; end > 0; end = end - 1) {
    const temporary = values[0]; values[0] = values[end]; values[end] = temporary;
    siftDown<T>(values, 0, end, compare);
  }
}
export func forEach<T>(values: T[], operation: (T) => void): void {
  for (let index = 0; index < values.length; index = index + 1) { operation(values[index]); }
}
export func any<T>(values: T[], predicate: (T) => bool): bool {
  for (let index = 0; index < values.length; index = index + 1) {
    if (predicate(values[index])) { return true; }
  }
  return false;
}
export func every<T>(values: T[], predicate: (T) => bool): bool {
  for (let index = 0; index < values.length; index = index + 1) {
    if (!predicate(values[index])) { return false; }
  }
  return true;
}
export func findIndex<T>(values: T[], predicate: (T) => bool): int {
  for (let index = 0; index < values.length; index = index + 1) {
    if (predicate(values[index])) { return index; }
  }
  return -1;
}
export func reduce<T, R>(values: T[], initial: R, operation: (R, T) => R): R {
  let result = initial;
  for (let index = 0; index < values.length; index = index + 1) {
    result = operation(result, values[index]);
  }
  return result;
}
export func transform<T>(values: T[], operation: (T) => T): void {
  for (let index = 0; index < values.length; index = index + 1) {
    values[index] = operation(values[index]);
  }
}
export func union<T>(left: Set<T>, right: Set<T>): Set<T> {
  const result: Set<T> = Set<T>();
  const first = left.values();
  const second = right.values();
  for (let index = 0; index < first.length; index = index + 1) { result.add(first[index]); }
  for (let index = 0; index < second.length; index = index + 1) { result.add(second[index]); }
  return result;
}
export func intersection<T>(left: Set<T>, right: Set<T>): Set<T> {
  const result: Set<T> = Set<T>();
  const values = left.values();
  for (let index = 0; index < values.length; index = index + 1) {
    if (right.has(values[index])) { result.add(values[index]); }
  }
  return result;
}
export func difference<T>(left: Set<T>, right: Set<T>): Set<T> {
  const result: Set<T> = Set<T>();
  const values = left.values();
  for (let index = 0; index < values.length; index = index + 1) {
    if (!right.has(values[index])) { result.add(values[index]); }
  }
  return result;
}
)";
  }
  return std::nullopt;
}

}  // namespace vela::frontend
