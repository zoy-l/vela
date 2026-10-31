#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/program_test_support.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"

namespace vela::compiler {
namespace {

class ByteDataProgramTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ByteDataProgramTest, ExecutesWithGCAndReleasesOwnedStorage) {
  runtime::Isolate isolate(1);
  const auto baseline = isolate.heap().object_count();
  {
    const auto result = test::RunInteger(GetParam(), isolate);
    ASSERT_TRUE(result) << result.error();
    EXPECT_EQ(*result, 42);
  }
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), baseline);
}

TEST_P(ByteDataProgramTest, ReloadsWithIndependentIsolateOwnership) {
  runtime::Isolate isolate(1);
  const auto loaded = test::ReloadSource(GetParam(), isolate);
  ASSERT_TRUE(loaded) << loaded.error();
  const auto result = test::ExecuteInteger(*loaded, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

INSTANTIATE_TEST_SUITE_P(ProductionContracts,
                         ByteDataProgramTest,
                         testing::Values(
                             R"(
func main(): int {
  let bytes = ByteArray.from([40]);
  const original = bytes;
  bytes[(bytes = ByteArray.from([1])).length - 1] += (bytes = ByteArray.from([2]))[0];
  let index = 0;
  const other = ByteArray.from([40,7]);
  other[index] += (index = 1) + 1;
  return original[0] == 42 && bytes[0] == 2 && other[0] == 42 && other[1] == 7 ? 42 : 0;
})",
                             R"(
type Bytes = ByteArray;
type Data = ByteData;
func main(): int {
  const empty = Bytes(length: 0);
  const bytes = Bytes(length: 8);
  const data = Data.view(bytes, length: 4);
  data.setInt32(0, 42);
  return empty.length == 0 && bytes[3] == 42 && data.byteLength == 4 ? 42 : 0;
})",
                             R"(
func main(): int {
  const source = [1, 2, 255];
  const bytes = ByteArray.from(source);
  const tail = bytes.slice(-2);
  const all = bytes.slice();
  all[0] = 0;
  source[1] = 0;
  return bytes[0] == 1 && tail[0] == 2 && bytes[1] == 2 && bytes.slice(3, 1).length == 0 ? 42 : 0;
})",
                             R"(
func main(): int {
  const bytes = ByteArray(24);
  const data = ByteData.view(bytes, offset: 1, length: 16);
  data.setInt8(0, -128);
  if (data.getInt8(0) != -128) { return 0; }
  data.setUint8(0, 255);
  data.setInt16(1, -32768);
  if (data.getInt16(1) != -32768 || data.getUint8(0) != 255) { return 0; }
  data.setUint16(1, 65535, endian: Endian.little);
  if (data.getUint16(1, endian: Endian.little) != 65535) { return 0; }
  data.setInt32(1, -2147483648);
  if (data.getInt32(1) != -2147483648) { return 0; }
  data.setUint32(1, 4294967295, endian: Endian.little);
  if (data.getUint32(1, endian: Endian.little) != 4294967295) { return 0; }
  data.setInt64(1, -9223372036854775808, endian: Endian.host);
  return data.getInt64(1, endian: Endian.host) == -9223372036854775808 ? 42 : 0;
})",
                             R"(
func main(): int {
  const data = ByteData.view(ByteArray(16));
  data.setFloat32(1, 1.5, endian: Endian.little);
  if (data.getFloat32(1, endian: Endian.little) != 1.5 || data.getUint32(1, endian: Endian.little) != 1069547520) { return 0; }
  data.setFloat64(1, -0.0);
  if (data.getInt64(1) != -9223372036854775808) { return 0; }
  data.setFloat64(1, double.parse('NaN'));
  return Math.isNaN(data.getFloat64(1)) ? 42 : 0;
})",
                             R"(
func main(): int {
  const bytes = ByteArray.from([40, 255]);
  const before = bytes[0]++;
  bytes[0] += 1;
  try { bytes[1]++; } catch (error: Error) {
    return error.code == 'InvalidNumericConversion' && before == 40 && bytes[0] == 42 && bytes[1] == 255 ? 42 : 0;
  }
  return 0;
})",
                             R"(
let order = 0;
func mark(value: int): int { order = order * 10 + value; return value; }
func main(): int {
  const bytes = ByteArray(12);
  const view = ByteData.view(length: mark(3), offset: mark(2), bytes: bytes);
  view.setUint16(value: mark(4), offset: mark(1), endian: Endian.little);
  return order == 3241 && bytes[3] == 4 && view.byteLength == 3 ? 42 : 0;
})",
                             R"(
func make(): ByteData { return ByteData.view(ByteArray.from([40,2]), offset: 0); }
class Box { data: ByteData; bytes: ByteArray; }
func identity<T>(value: T): T { return value; }
func main(): int {
  const box = Box(data: make(), bytes: ByteArray(0));
  const list: ByteData?[] = [null, box.data];
  const map = Map<string, ByteData>();
  map.set('view', identity<ByteData>(box.data));
  for (let index = 0; index < 200; index++) { const temporary = ByteArray(1024); }
  const saved = map.get('view');
  return saved?.getUint8(0) == 40 && box.data.getUint8(1) == 2 && list[1]?.byteLength == 2 ? 42 : 0;
})",
                             R"(
let effects = 0;
func position(): int { effects++; return 0; }
func main(): int {
  const bytes: ByteArray? = null;
  const data: ByteData? = null;
  const missing = bytes?.[position()];
  const size = bytes?.length;
  const read = data?.getUint16(position());
  data?.setUint8(position(), position());
  const copy = bytes?.slice(position());
  return missing == null && size == null && read == null && copy == null && effects == 0 ? 42 : 0;
})",
                             R"(
func main(): int {
  const from: (int[]) => ByteArray = ByteArray.from;
  const view: (ByteArray, int, int) => ByteData = ByteData.view;
  const bytes = from([0,0,0,0]);
  const data = view(bytes, 0, 4);
  const set: (int, int) => void = data.setInt32;
  const get: (int) => int = data.getInt32;
  const slice: () => ByteArray = bytes.slice;
  set(0, 42);
  const copy = slice();
  copy[3] = 0;
  return get(0) == 42 && bytes[3] == 42 ? 42 : 0;
})",
                             R"(
func main(): int {
  const bytes = ByteArray.from([1,2,3,4]);
  const first = ByteData.view(bytes);
  const second = ByteData.view(bytes, offset: 1);
  first.setUint8(1, 42);
  if (second.getUint8(0) != 42) { return 0; }
  second.setInt16(0, 258);
  return first.getUint8(1) == 1 && first.getUint8(2) == 2 ? 42 : 0;
})",
                             R"(
func main(): int {
  const bytes = ByteArray.from([7,7,7,7]);
  const data = ByteData.view(bytes);
  let errors = 0;
  try { data.setUint32(1, 0); } catch (error: Error) { if (error.code == 'IndexOutOfBounds') { errors++; } }
  try { data.setInt16(0, 32768); } catch (error: Error) { if (error.code == 'InvalidNumericConversion') { errors++; } }
  try { ByteArray.from([0,256]); } catch (error: Error) { if (error.code == 'InvalidNumericConversion') { errors++; } }
  try { ByteArray(-1); } catch (error: Error) { if (error.code == 'InvalidNumericConversion') { errors++; } }
  try { ByteData.view(bytes, length: -1); } catch (error: Error) { if (error.code == 'IndexOutOfBounds') { errors++; } }
  return errors == 5 && data.getUint32(0) == 117901063 ? 42 : 0;
})",
                             R"(
func main(): int {
  const a = ByteArray(0);
  const b = a;
  const view = ByteData.view(a);
  const nullable: ByteData? = view;
  return a == b && a != a.slice() && view != ByteData.view(a) && (nullable is ByteData) ? 42 : 0;
})"));

class InvalidByteDataTest : public testing::TestWithParam<std::string_view> {};

TEST_P(InvalidByteDataTest, RejectsWrongTypesReadonlyPropertiesAndUnsupportedOperations) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(GetParam(), isolate);
  EXPECT_FALSE(compiled.Succeeded());
  EXPECT_FALSE(compiled.diagnostics.empty());
}

INSTANTIATE_TEST_SUITE_P(
    StaticContracts,
    InvalidByteDataTest,
    testing::Values(
        "func main(): void { ByteArray(1.0); }",
        "func main(): void { ByteArray.from([1.0]); }",
        "func main(): void { const a: int?[] = [null]; ByteArray.from(a); }",
        "func main(): void { const a: int[] = ByteArray(8); }",
        "func main(): void { const a: ByteArray = [1,2]; }",
        "func main(): void { ByteData(); }",
        "func main(): void { ByteData.view([1,2]); }",
        "func main(): void { ByteData.view(ByteArray(1), offset: null); }",
        "func main(): void { ByteData.view(ByteArray(1), length: null); }",
        "func main(): void { const b = ByteArray(8); b.length = 0; }",
        "func main(): void { const d = ByteData.view(ByteArray(8)); d.byteLength = 0; }",
        "func main(): void { const b = ByteArray(8); b[0] = 1.0; }",
        "func main(): void { const b = ByteArray(8); print(b[0.0]); }",
        "func main(): void { ByteArray(8).push(1); }",
        "func main(): void { ByteArray(8).reserve(16); }",
        "func main(): void { const d = ByteData.view(ByteArray(8)); print(d[0]); }",
        "func main(): void { ByteData.view(ByteArray(8)).getInt32(0, endian: 1); }",
        "func main(): void { ByteData.view(ByteArray(8)).getInt32(0, endian: null); }",
        "func main(): void { ByteData.view(ByteArray(8)).getInt8(0, endian: Endian.big); }",
        "func main(): void { ByteData.view(ByteArray(8)).setFloat32(0, 1); }",
        "func main(): void { ByteData.view(ByteArray(8)).getUint64(0); }",
        "func main(): void { const d: ByteData? = null; d.getUint8(0); }",
        "class ByteArray {} func main(): void {}",
        "enum Endian { fake } func main(): void {}"));

}  // namespace
}  // namespace vela::compiler
