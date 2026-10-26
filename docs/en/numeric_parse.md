# Parsing numbers from strings

[**English**](numeric_parse.md) | [简体中文](../zh-CN/numeric_parse.md)

`int.parse`, `int.tryParse`, `double.parse`, and `double.tryParse` convert complete
strings to native numbers. The API uses Dart-style names, signed 64-bit Vela
integers, and locale-independent conversion.

```vela
func main(): void {
  print(int.parse('42'));
  print(int.parse('ff', radix: 16));
  const missing: int? = int.tryParse('unknown');
  print(missing ?? 0);
  print(double.parse('1.25e2'));
}
```

| API | Result |
| --- | --- |
| `int.parse(source: string, radix?: int?): int` | Return an int or throw Error |
| `int.tryParse(source: string, radix?: int?): int?` | Return null for invalid text or int64 overflow |
| `double.parse(source: string): double` | Return binary64 or throw Error |
| `double.tryParse(source: string): double?` | Return null for invalid text; NaN is a valid result |

No import is required. Type aliases, positional/named arguments, checking,
documentation, and editor queries share these signatures. Local variables follow
ordinary shadowing rules. Method values have fixed positional signatures:
the int.parse value is `(string, int?) => int`, with null requesting automatic
radix selection. Numbers and null source values are not implicitly converted.

Both parsers ignore only leading and trailing ASCII whitespace: space and
`\t`, `\n`, `\v`, `\f`, `\r`. Other Unicode whitespace is not ignored. The
remaining text must match completely. Partial parsing, interior whitespace,
underscores, non-ASCII digits, embedded NUL, trailing data, empty strings, and
whitespace-only input are rejected.

## Integers

One leading `+` or `-` is allowed. With omitted/null radix, decimal is the
default; `0x` or `0X` after the sign selects hexadecimal. An explicit radix
must be 2..36 and uses case-insensitive alphanumeric digits. Explicit radices
do not specially recognize a `0x` prefix: every character must be a digit valid
in that radix (`0xff` is invalid in radix 16; `0x` is valid digit text in radix 36).
Binary/octal prefixes are not detected,
and leading zeroes do not change the radix.

The result must be within `[-9223372036854775808, 9223372036854775807]`.
Overflow is checked before multiplication/addition, without truncation,
wrapping, or interpreting an unsigned hexadecimal value as negative.
For example, `0xffffffffffffffff` is out of range. Integer `-0` becomes zero;
decimal points and exponents are not integer syntax.

## Floating-point numbers

Decimal syntax consists of an optional sign, a mantissa, and an optional `e` /
`E` exponent. At least one mantissa digit is required; `.5` and `1.` are valid.
The exponent may have a sign but must contain digits. Case-sensitive `NaN` and
`Infinity` are also accepted, each with an optional sign. Hexadecimal floats,
`inf`, lowercase `nan`, and NaN payload syntax are rejected.

Existing Abseil conversion supplies IEEE 754 rounding, including subnormal
values and signed zero. Overflowing decimal text produces signed Infinity;
underflow rounds to a subnormal or signed zero. These remain valid results
of tryParse. NaN payload bits are unspecified. Use Math.isFinite when a finite
result is required.

## Errors and lifetime

| Failure | Contract |
| --- | --- |
| Invalid/overflowing int.parse text | `Error(code: 'IntParseError')` |
| Invalid double.parse text | `Error(code: 'DoubleParseError')` |
| Explicit radix outside 2..36 | `Error(code: 'InvalidRadix')`, including tryParse |
| Wrong static argument type, name, or count | Frontend diagnostic |
| Error allocation failure | Existing OutOfMemory path |

Failures use ordinary catch/finally and retain source traces. Diagnostic wording
is separate from stable error codes. tryParse converts only invalid text to null;
it does not swallow argument-evaluation errors or invalid radices. Successful
results are native int/double values without numeric wrapper allocation.
Parsing neither modifies nor retains source strings. Standard methods execute
through builtin bytecode; declarations and implementation participate in cache
identity. See [numeric_parse.vela](../../examples/numeric_parse.vela).

The recorded Windows x64 Ryzen 9 7900X Release run measured median steady
latencies of 270 ns for int64 decimal, 241 ns for hexadecimal, 268 ns for
double, and 238 ns for invalid int.tryParse. Timing includes VM execution;
these cases reported zero first-call GC allocation and zero additional retained
heap bytes after collection. See [measurement boundaries](../../benchmarks/README.md)
and [exact aggregate evidence](../../benchmarks/results/rolling_summary.json).
