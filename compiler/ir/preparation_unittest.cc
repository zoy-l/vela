#include "compiler/ir/preparation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "base/builtin.h"
#include "base/layout_description.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::ir {
namespace {

using test::Prepare;

template <class Verify>
void WithPreparedModes(std::string_view source, const Verify& verify) {
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  auto parsed = frontend::LoadSources(std::span(&input, 1), input.path);
  ASSERT_TRUE(parsed.diagnostics.empty()) << frontend::DiagnosticsText(parsed.diagnostics);
  const auto checked = frontend::TypeChecker().Check(parsed.program);
  ASSERT_TRUE(checked.Succeeded()) << frontend::DiagnosticsText(checked.diagnostics);
  for (const auto mode : {NativeProgramMode::Closed, NativeProgramMode::Reloadable}) {
    frontend::DeclarationIds identities;
    const auto prepared = PrepareProgram(parsed.program, checked.model,
                                         {.declarations = &identities, .native_mode = mode});
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    ASSERT_NO_FATAL_FAILURE(verify(prepared.program, mode));
  }
}

TEST(PreparationTest, BuiltinExpressionsDeclarePreviouslyUnrecordedResultTypes) {
  WithPreparedModes(
      R"(
export func convert(value: int): double { return value.toDouble(); }
)",
      [](const Program& program, NativeProgramMode) {
        const auto& functions = program.functions;
        const auto convert = std::ranges::find(functions, "convert", &Function::name);
        ASSERT_NE(convert, functions.end());
        const auto conversion =
            std::ranges::find_if(convert->instructions, [](const Instruction& instruction) {
              return instruction.opcode == Opcode::BuiltinCall &&
                     instruction.index == static_cast<std::uint32_t>(Builtin::NumericToDouble);
            });
        ASSERT_NE(conversion, convert->instructions.end());
        EXPECT_EQ(convert->value_types.at(conversion->dst).kind,
                  frontend::TypeDescriptor::Kind::Double);
      });
}

TEST(PreparationTest, StructValuesRetainLayoutsAndFieldWritesDefineUpdatedRegisters) {
  WithPreparedModes(R"(
struct Position { x: double; y: double; }
struct Body { position: Position; active: bool; }
func copy(value: Body): Body { return value; }
export func run(): double {
  let body = Body(position: Position(x: 1.0, y: 2.0), active: true);
  body.active = false;
  const snapshot = copy(body);
  return snapshot.position.x;
}
)",
                    [](const Program& program, NativeProgramMode) {
                      const auto body = std::ranges::find(program.classes, "Body", &Class::name);
                      ASSERT_NE(body, program.classes.end());
                      EXPECT_EQ(body->kind, Class::Kind::Struct);
                      ASSERT_NE(body->value_layout, nullptr);
                      EXPECT_EQ(body->value_layout->slots.size(), 3U);
                      const auto index = static_cast<std::size_t>(body - program.classes.begin());
                      EXPECT_EQ(program.layouts.classes[index].kind, layout::Class::Kind::Struct);
                      const auto copy =
                          std::ranges::find(program.functions, "copy", &Function::name);
                      ASSERT_NE(copy, program.functions.end());
                      ASSERT_NE(copy->return_type.value_layout, nullptr);
                      EXPECT_EQ(copy->value_types.front().value_layout, body->value_layout);
                      const auto run = std::ranges::find(program.functions, "run", &Function::name);
                      ASSERT_NE(run, program.functions.end());
                      std::size_t writes = 0;
                      for (const auto& instruction : run->instructions) {
                        if (instruction.opcode == Opcode::NewInstance) {
                          EXPECT_NE(run->value_types[instruction.dst].value_layout, nullptr);
                          EXPECT_TRUE(instruction.inline_value);
                          EXPECT_FALSE(AnalyzeEffects(*run, instruction).MayCollect());
                        }
                        if (instruction.opcode != Opcode::SetField) {
                          continue;
                        }
                        ++writes;
                        EXPECT_TRUE(instruction.inline_value);
                        EXPECT_TRUE(AnalyzeRegisters(instruction, program.functions).defines_value);
                      }
                      EXPECT_GT(writes, 0U);
                    });
}

TEST(PreparationTest, StructMethodsKeepWritableReceiversSeparateFromReadonlyCopies) {
  WithPreparedModes(
      R"(
struct Counter { value: int; add(): void { this.value++; } }
export func run(): int {
  let value = Counter(value: 1);
  const frozen = value;
  value.add();
  frozen.add();
  return value.value;
}

)",
      [](const Program& program, NativeProgramMode) {
        const auto run = std::ranges::find(program.functions, "run", &Function::name);
        ASSERT_NE(run, program.functions.end());
        std::size_t borrowed = 0;
        for (const auto& instruction : run->instructions) {
          borrowed += instruction.struct_receiver != nullptr ? 1U : 0U;
          if (instruction.struct_receiver) {
            EXPECT_EQ(instruction.opcode, Opcode::Call);
          }
        }
        EXPECT_EQ(borrowed, 1U);
        const auto method = std::ranges::find_if(
            program.functions, [](const auto& function) { return function.struct_receiver; });
        ASSERT_NE(method, program.functions.end());
        EXPECT_TRUE(std::ranges::any_of(method->instructions, [&](const auto& instruction) {
          return AnalyzeRegisters(instruction, program.functions).defines_value &&
                 instruction.dst == 0 && AnalyzeEffects(*method, instruction).writes_state;
        })) << method->name;
      });
}

TEST(PreparationTest, StructAssignmentsReadOnlyTheFieldsNeededForWriteback) {
  WithPreparedModes(
      R"(
struct Point { x: int; y: int; }
struct Body { point: Point; other: int; }
export func direct(point: Point, value: int): Point { point.x = value; return point; }
export func nested(body: Body, value: int): Body { body.point.x = value; return body; }
)",
      [](const Program& program, NativeProgramMode) {
        const auto& functions = program.functions;
        for (const auto& [name, reads] : {std::pair{"direct", 0}, std::pair{"nested", 1}}) {
          SCOPED_TRACE(name);
          const auto update = std::ranges::find(functions, name, &Function::name);
          ASSERT_NE(update, functions.end());
          const auto& instructions = update->instructions;
          EXPECT_EQ(std::ranges::count(instructions, Opcode::GetField, &Instruction::opcode),
                    reads);
          EXPECT_EQ(std::ranges::count(instructions, Opcode::SetField, &Instruction::opcode),
                    reads + 1);
        }
      });
}

TEST(PreparationTest, CallerInitializedNullDefaultsDoNotNeedStructConstructorCalls) {
  WithPreparedModes(
      R"(
func initial(): int { return 1; }
struct Entry { value: int = initial(); text: string? = null; }
export func update(value: int): Entry { return Entry(value: value); }
)",
      [](const Program& program, NativeProgramMode) {
        const auto update = std::ranges::find(program.functions, "update", &Function::name);
        ASSERT_NE(update, program.functions.end());
        EXPECT_EQ(std::ranges::count(update->instructions, Opcode::Call, &Instruction::opcode), 0);
        EXPECT_EQ(std::ranges::count(update->instructions, Opcode::SetField, &Instruction::opcode),
                  2);
      });
}

TEST(PreparationTest, StructAndClassCanonicalTypesCannotShareAnAbiKey) {
  const layout::Type type{.kind = layout::Type::Kind::Class};
  layout::Program reference;
  layout::Class nominal;
  nominal.name = "Position";
  reference.classes.push_back(nominal);
  auto value = reference;
  value.classes.front().kind = layout::Class::Kind::Struct;
  EXPECT_NE(layout::TypeKey(reference, type), layout::TypeKey(value, type));
  EXPECT_FALSE(layout::IsSameType(reference, type, value, type));
}

TEST(PreparationTest, PredicateOutcomesParticipateInCanonicalTypeIdentity) {
  const layout::Type type{.kind = layout::Type::Kind::Function};
  layout::Program original;
  layout::Signature signature;
  signature.result = layout::Type{.kind = layout::Type::Kind::Bool};
  signature.narrowed_parameter = layout::Type{.kind = layout::Type::Kind::Null};
  signature.false_narrowed_parameter = layout::Type{.kind = layout::Type::Kind::Int};
  original.signatures.push_back(signature);
  auto erased = original;
  erased.signatures.front().false_narrowed_parameter.reset();
  EXPECT_NE(layout::TypeKey(original, type), layout::TypeKey(erased, type));
  EXPECT_FALSE(layout::IsSameType(original, type, erased, type));
  layout::Program imported;
  const auto copied = layout::ImportType(imported, original, type);
  EXPECT_TRUE(layout::IsSameType(original, type, imported, copied));
  EXPECT_EQ(layout::TypeKey(original, type), layout::TypeKey(imported, copied));
}

TEST(PreparationTest, IntrinsicOperationWindowsPreserveSdkParameters) {
  const auto prepared = Prepare("import 'sdk:mock' as mock; func main(): void {}");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  constexpr std::array kBuiltins{
      Builtin::MockInspect,
      Builtin::TestInspect,
      Builtin::TestControl,
  };
  std::array<std::size_t, kBuiltins.size()> operations{};
  const auto builtins = std::span(kBuiltins);
  for (const auto& function : prepared.program.functions) {
    for (const auto& instruction : function.instructions) {
      const auto builtin = static_cast<Builtin>(instruction.index);
      const auto selected = std::ranges::find(builtins, builtin);
      if (instruction.opcode != Opcode::BuiltinCall || selected == builtins.end()) {
        continue;
      }
      const auto slot = static_cast<std::size_t>(selected - builtins.begin());
      ++operations[slot];
      EXPECT_GE(instruction.lhs, function.parameter_count) << function.name;
    }
  }
  for (const auto count : operations) {
    EXPECT_GT(count, 0U);
  }
}

TEST(PreparationTest, ConstructorConversionsOnlyCheckActualTypeBoundaries) {
  WithPreparedModes(R"(
enum Choice { first }
export class Box {
  value: int;
  maybe: int? = null;
  choice: Choice = Choice.first;
  constructor(this.value, this.maybe, this.choice);
}
export class Fields { value: int; maybe: int? = null; }
export func make(): Box { return Box(42); }
export func checked(value: dynamic): int { return value as int; }
)",
                    [](const Program& program, NativeProgramMode) {
                      bool has_dynamic_check = false;
                      for (const auto& function : program.functions) {
                        for (const auto& instruction : function.instructions) {
                          if (instruction.opcode != Opcode::CheckedCast) {
                            continue;
                          }
                          const auto& input = function.value_types[instruction.lhs];
                          const auto& target = program.runtime_types[instruction.index];
                          EXPECT_FALSE(input.kind == target.kind && input.name == target.name &&
                                       input.nullable == target.nullable)
                              << function.name;
                          has_dynamic_check =
                              has_dynamic_check ||
                              (input.kind == frontend::TypeDescriptor::Kind::Dynamic &&
                               target.kind == frontend::TypeDescriptor::Kind::Int);
                        }
                      }
                      EXPECT_TRUE(has_dynamic_check);
                    });
}

TEST(PreparationTest, ParameterlessBaseCallsDoNotMaterializeArgumentConstants) {
  WithPreparedModes(
      R"(
class Base { constructor() {} }
export class Child extends Base { constructor() {} }
)",
      [](const Program& program, NativeProgramMode) {
        const auto child = std::ranges::find_if(program.functions, [&](const auto& function) {
          return function.signature && function.signature->receiver &&
                 program.runtime_types[*function.signature->receiver].name == "Child";
        });
        ASSERT_NE(child, program.functions.end());
        EXPECT_EQ(std::ranges::count(child->instructions, Opcode::LoadBool, &Instruction::opcode),
                  0);
        EXPECT_EQ(std::ranges::count(child->instructions, Opcode::LoadNull, &Instruction::opcode),
                  1);
      });
}

TEST(PreparationTest, StaticBaseFieldPresenceSkipsAbsentConversionsAndRuntimeBranches) {
  WithPreparedModes(
      R"(
class Base { value: int; optional: int = 2; }
export class Child extends Base { constructor(super.value); }
)",
      [](const Program& program, NativeProgramMode) {
        const auto child = std::ranges::find_if(program.functions, [&](const auto& function) {
          return function.signature && function.signature->receiver &&
                 program.runtime_types[*function.signature->receiver].name == "Child";
        });
        ASSERT_NE(child, program.functions.end());
        EXPECT_EQ(std::ranges::count(child->instructions, Opcode::SetField, &Instruction::opcode),
                  1);
        for (const auto opcode :
             {Opcode::JumpIfFalse, Opcode::CheckedCast, Opcode::LoadBool, Opcode::BitOrInt}) {
          EXPECT_EQ(std::ranges::count(child->instructions, opcode, &Instruction::opcode), 0);
        }
      });
}

TEST(PreparationTest, MergedAccessorsKeepSeparateGetterAndSetterParameterShapes) {
  WithPreparedModes(R"(
class Box {
  value: int = 42;
  get amount(): int { return this.value; }
  set amount(next: int): void { this.value = next; }
}
)",
                    [](const Program& program, NativeProgramMode) {
                      std::size_t accessors = 0;
                      for (const auto& function : program.functions) {
                        if (!function.signature || !function.name.contains("amount")) {
                          continue;
                        }
                        const auto& signature = *function.signature;
                        const std::size_t parameters = signature.result ? 0 : 1;
                        EXPECT_EQ(signature.parameters.size(), parameters) << function.name;
                        EXPECT_EQ(function.parameter_count,
                                  parameters + signature.hidden_parameters);
                        ++accessors;
                      }
                      EXPECT_EQ(accessors, 2U);
                    });
}

TEST(PreparationTest, SpecializesOnlyProvenClosedIntegerSortOrders) {
  struct Case final {
    std::string_view comparator;
    std::optional<Builtin> builtin;
  };
  constexpr std::array kCases{
      Case{
          .comparator = "(a, b) => a < b ? -1 : (a > b ? 1 : 0)",
          .builtin = Builtin::ArraySortIntAscending,
      },
      Case{
          .comparator = "(a, b) => a > b ? -2 : (a < b ? 3 : 0)",
          .builtin = Builtin::ArraySortIntDescending,
      },
      Case{
          .comparator = "(a, b) => b > a ? -1 : (b < a ? 1 : 0)",
          .builtin = Builtin::ArraySortIntAscending,
      },
      Case{.comparator = "(a, b) => a - b", .builtin = std::nullopt},
      Case{.comparator = "(a, b) => a < b ? -1 : 1", .builtin = std::nullopt},
      Case{.comparator = "(a, b) => a == 0 ? -1 : (a > b ? 1 : 0)", .builtin = std::nullopt},
      Case{
          .comparator = "(a, b) => { calls++; return a < b ? -1 : (a > b ? 1 : 0); }",
          .builtin = std::nullopt,
      },
      Case{.comparator = "(a, b) => a < limit ? -1 : (a > b ? 1 : 0)", .builtin = std::nullopt},
  };
  for (const auto& entry : kCases) {
    SCOPED_TRACE(entry.comparator);
    const auto source =
        "let calls = 0; const limit = 1; func run(values: int[]): int[] { return values.sort(" +
        std::string(entry.comparator) + "); }";
    ASSERT_NO_FATAL_FAILURE(
        WithPreparedModes(source, [&](const Program& program, NativeProgramMode mode) {
          const auto found = std::ranges::find(program.functions, "run", &Function::name);
          ASSERT_NE(found, program.functions.end());
          std::size_t specialized = 0;
          std::size_t callbacks = 0;
          for (const auto& instruction : found->instructions) {
            callbacks += static_cast<std::size_t>(instruction.opcode == Opcode::CallIndirect);
            if (instruction.opcode != Opcode::BuiltinCall) {
              continue;
            }
            const auto builtin = static_cast<Builtin>(instruction.index);
            if (builtin == Builtin::ArraySortIntAscending ||
                builtin == Builtin::ArraySortIntDescending) {
              ++specialized;
              ASSERT_TRUE(entry.builtin);
              EXPECT_EQ(builtin, *entry.builtin);
            }
          }
          EXPECT_EQ(specialized, mode == NativeProgramMode::Closed && entry.builtin ? 1U : 0U);
          // 特化也保留一次原比较器调用，覆盖非平凡输入的深度及控制请求边界。
          EXPECT_GT(callbacks, 0U);
        }));
  }
}

TEST(PreparationTest, FieldIdentitiesApplyOnlyToReloadableInstances) {
  constexpr std::string_view kSource = R"(
class Box { value: int; }
func read(box: Box, error: Error, parts: DateParts): int {
  return box.value + error.code.length + parts.millisecond;
}
)";
  ASSERT_NO_FATAL_FAILURE(
      WithPreparedModes(kSource, [&](const Program& program, NativeProgramMode mode) {
        std::size_t instance_reads = 0;
        std::size_t fixed_reads = 0;
        for (const auto& function : program.functions) {
          for (const auto& instruction : function.instructions) {
            if (instruction.opcode != Opcode::GetField) {
              continue;
            }
            const auto name = function.value_types[instruction.lhs].name;
            if (name == "Box") {
              ++instance_reads;
              EXPECT_EQ(instruction.field_declaration.has_value(),
                        mode == NativeProgramMode::Reloadable);
            } else if (name == "Error" || name == "DateParts") {
              ++fixed_reads;
              EXPECT_FALSE(instruction.field_declaration);
            }
          }
        }
        EXPECT_GT(instance_reads, 0U);
        EXPECT_GT(fixed_reads, 1U);
      }));
}

TEST(PreparationTest, EnumRuntimeNamesDoNotBorrowSourceDeclarations) {
  const frontend::SourceInput input{
      .path = "main.vela",
      .text = "enum Short { first } enum LongEnumerationNameBeyondInlineStorage { second }",
  };
  auto parsed = frontend::LoadSources(std::span(&input, 1), input.path);
  ASSERT_TRUE(parsed.diagnostics.empty());
  auto checked = frontend::TypeChecker().Check(parsed.program);
  ASSERT_TRUE(checked.Succeeded());
  const auto prepared = PrepareProgram(parsed.program, checked.model);
  ASSERT_TRUE(prepared.Succeeded());
  for (auto& enumeration : parsed.program.enums) {
    const auto name = enumeration.name;
    enumeration.name.front() = 'X';
    const auto& types = prepared.program.runtime_types;
    const auto type = std::ranges::find(types, name, &frontend::TypeDescriptor::name);
    ASSERT_NE(type, types.end()) << name;
    EXPECT_EQ(type->kind, frontend::TypeDescriptor::Kind::Enum);
  }
}

TEST(PreparationTest, ScalarValueAwaitSkipsOnlyAnUnobservableFactory) {
  const auto prepared = Prepare(R"(
func keep(value: Future<int>): Future<int> { return value; }
async func integer(value: int): Future<int> { return await Future.value(value); }
async func decimal(value: double): Future<double> { return await Future.value(value); }
async func boolean(value: bool): Future<bool> { return await Future.value(value); }
async func nullable(value: int?): Future<int?> { return await Future.value(value); }
async func named(value: int): Future<int> {
  const ready = Future.value(value); return await ready;
}
async func multiple(value: int): Future<int> {
  const ready = Future.value(value); return await ready + await ready;
}
async func escaped(value: int): Future<int> { return await keep(Future.value(value)); }
async func adopted(value: Future<int>): Future<int> { return await Future.value(value); }
async func erased(value: dynamic): Future<int> { return await Future.value<int>(value); }
async func failed(value: Error): Future<int> { return await Future.error<int>(value); }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  constexpr std::array<std::string_view, 6> kPreserved{
      "named", "multiple", "escaped", "adopted", "erased", "failed",
  };
  for (const auto& function : prepared.program.functions) {
    const auto factories =
        std::ranges::count(function.instructions, Opcode::NewFuture, &Instruction::opcode);
    EXPECT_EQ(factories, std::ranges::contains(kPreserved, function.name) ? 1 : 0) << function.name;
  }
}

TEST(PreparationTest, FusesOnlyUnobservedScalarTextUsedForItsLength) {
  const auto prepared = Prepare(R"(
func length(value: double): int { const text = value.toString(); return text.length; }
func bytes(value: int): int { return value.toString().byteLength; }
func observed(value: int): int { const text = value.toString(); print(text); return text.length; }
func reassigned(value: int): int { let text = value.toString(); text = 'kept'; return text.length; }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  for (const auto& function : prepared.program.functions) {
    const auto fused = std::ranges::count_if(function.instructions, [](const auto& instruction) {
      return instruction.opcode == Opcode::BuiltinCall &&
             instruction.index == static_cast<std::uint32_t>(Builtin::ScalarTextLength);
    });
    EXPECT_EQ(fused, function.name == "length" || function.name == "bytes" ? 1 : 0)
        << function.name;
  }
}

TEST(PreparationTest, FusesOnlyIndexedStringsWhoseSingleConsumerAppendsThem) {
  const auto prepared = Prepare(R"(
func direct(text: string, index: int): string {
  const builder = String(); builder.append(text[index]); return builder.toString();
}
func delayed(text: string, index: int): string {
  const builder = String(); const point = text[index]; const copy = point;
  text = 'replaced'; builder.append(copy); return builder.toString();
}
func observed(text: string, index: int): string {
  const builder = String(); const point = text[index]; print(point);
  builder.append(point); return builder.toString();
}
func reassigned(text: string, index: int): string {
  const builder = String(); let point = text[index]; point = 'kept';
  builder.append(point); return builder.toString();
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  for (const auto& function : prepared.program.functions) {
    const auto expected = function.name == "direct" || function.name == "delayed" ? 1 : 0;
    for (const auto builtin : {Builtin::StringCodePointAt, Builtin::MutableStringAppendCodePoint}) {
      EXPECT_EQ(std::ranges::count_if(function.instructions,
                                      [builtin](const auto& instruction) {
                                        return instruction.opcode == Opcode::BuiltinCall &&
                                               instruction.index ==
                                                   static_cast<std::uint32_t>(builtin);
                                      }),
                expected)
          << function.name;
    }
  }
}

TEST(PreparationTest, PreservesUnboundNativeImportsWithoutRuntimeState) {
  auto result = Prepare(R"(
external func read(): int = "host.read";
func main(): int { return read() + 2; }
)");
  ASSERT_TRUE(result.Succeeded());
  const auto& functions = result.program.functions;
  const auto found = std::ranges::find(functions, "host.read", &Function::native_symbol);
  ASSERT_NE(found, functions.end());
  ASSERT_TRUE(found->signature.has_value());
  EXPECT_TRUE(found->instructions.empty());
  EXPECT_EQ(found->return_type.kind, frontend::TypeDescriptor::Kind::Int);
}

TEST(PreparationTest, RuntimeInterfaceLayoutsExcludeAbstractTemplateApplications) {
  const auto prepared = Prepare(R"(
interface Reader<T> { read(): T; }
class Value implements Reader<int> { read(): int { return 42; } }
class Operations {
  read<T extends Reader<int>>(value: T): int { return value.read(); }
}
func main(): int { return Operations().read(Value()); }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto& interfaces = prepared.program.layouts.interfaces;
  ASSERT_EQ(interfaces.size(), 1U);
  layout::Program arguments;
  const layout::Nominal nominal{
      .module = "main.vela",
      .name = "Reader",
      .arguments = {{.kind = layout::Type::Kind::Int}},
  };
  EXPECT_EQ(interfaces.front(), layout::NominalKey(arguments, nominal));
}

TEST(PreparationTest, KeepsWideVirtualRegistersBeforeBackendAllocation) {
  constexpr int kArrayLength = 300;
  std::string source = "func main(): int { const values = [";
  for (int index = 0; index < kArrayLength; ++index) {
    if (index != 0) {
      source += ',';
    }
    source += std::to_string(index);
  }
  source += "]; return values[299]; }";
  auto result = Prepare(source);
  ASSERT_TRUE(result.Succeeded());
  const auto& functions = result.program.functions;
  const auto main = std::ranges::find(functions, "main", &Function::name);
  ASSERT_NE(main, functions.end());
  EXPECT_GT(main->register_count, 256U);
  EXPECT_TRUE(std::ranges::any_of(main->instructions, [](const Instruction& instruction) {
    return instruction.opcode == Opcode::NewArray && instruction.immediate == kArrayLength;
  }));
}

TEST(PreparationTest, ClassInterfaceMetadataHasCanonicalOrder) {
  constexpr std::uint32_t kInterfaces = 8;
  std::string source;
  for (std::uint32_t index = 0; index < kInterfaces; ++index) {
    source += "interface I" + std::to_string(index) + " { read(): int; }";
  }
  source += "class Value implements ";
  for (std::uint32_t index = kInterfaces; index > 0; --index) {
    source += "I" + std::to_string(index - 1) + (index == 1 ? "" : ",");
  }
  source += "{ read(): int { return 42; } } func main(): int { return Value().read(); }";
  const auto prepared = Prepare(source);
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& classes = prepared.program.layouts.classes;
  const auto key = layout::NominalKey(prepared.program.layouts,
                                      {.module = "main.vela", .name = "Value", .arguments = {}});
  const auto found = std::ranges::find(classes, key, &layout::Class::name);
  ASSERT_NE(found, classes.end());
  ASSERT_EQ(found->interfaces.size(), kInterfaces);
  for (std::uint32_t index = 0; index < kInterfaces; ++index) {
    EXPECT_EQ(found->interfaces.at(index), index);
  }
}

TEST(PreparationTest, OwnsLayoutsAndSourcePathsAfterFrontendDestructionAndResultMoves) {
  auto first = Prepare(R"(
class Box { value: int; }
func main(): int { return Box(value: 42).value; }
)");
  ASSERT_TRUE(first.Succeeded());
  auto prepared = std::move(first.program);
  first = {};
  const auto& classes = prepared.layouts.classes;
  const auto key =
      layout::NominalKey(prepared.layouts, {.module = "main.vela", .name = "Box", .arguments = {}});
  const auto found = std::ranges::find(classes, key, &layout::Class::name);
  ASSERT_NE(found, classes.end());
  EXPECT_EQ(found->field_count, 1U);
  EXPECT_TRUE(found->reference_fields.empty());
  EXPECT_FALSE(prepared.classes.empty());
  EXPECT_TRUE(std::ranges::any_of(
      prepared.source_paths, [](const std::string& path) { return path.ends_with("main.vela"); }));
}

TEST(PreparationTest, ReportsImportedTypeErrorsBeforeLowering) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'library'; func main(): int { return answer(); }"},
      {.path = "library.vela", .text = "export func answer(): int { return true; }"},
  });
  const auto result = PrepareParsed(frontend::LoadSources(sources, "main.vela"));
  ASSERT_FALSE(result.Succeeded());
  ASSERT_FALSE(result.diagnostics.empty());
  EXPECT_TRUE(result.diagnostics.front().source_path.ends_with("library.vela"));
}

TEST(PreparationTest, EmitsFieldDefaultsOnceInTheConstructorAcrossManyCallSites) {
  std::string source =
      "external func initial(): int; class Box { value: int = initial(); } "
      "func main(): void {";
  constexpr int kConstructions = 50;
  for (int index = 0; index < kConstructions; ++index) {
    source += "Box();";
  }
  source += '}';
  const auto prepared = Prepare(source);
  ASSERT_TRUE(prepared.Succeeded());
  const auto& functions = prepared.program.functions;
  const auto initial = std::ranges::find(functions, "initial", &Function::name);
  ASSERT_NE(initial, functions.end());
  const auto target = static_cast<std::uint32_t>(initial - functions.begin());
  std::size_t default_calls = 0;
  for (const auto& function : functions) {
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::Call && instruction.index == target) {
        ++default_calls;
        EXPECT_EQ(function.name, "Box.$constructor");
      }
    }
  }
  EXPECT_EQ(default_calls, 1U);
}

TEST(PreparationTest, UnusedConstructorsDoNotIntroduceBodiesOrStringRoots) {
  for (const auto* const body : {"", "constructor {}", "constructor() {}"}) {
    SCOPED_TRACE(body);
    const auto prepared = Prepare("class Unused { value: string = 'unused-constructor-literal'; " +
                                  std::string(body) + " } func main(): int { return 42; }");
    ASSERT_TRUE(prepared.Succeeded());
    EXPECT_FALSE(std::ranges::any_of(prepared.program.functions, [](const Function& function) {
      return function.name == "Unused.$constructor";
    }));
    EXPECT_EQ(std::ranges::find(prepared.program.strings, "unused-constructor-literal"),
              prepared.program.strings.end());
  }
}

}  // namespace
}  // namespace vela::ir
