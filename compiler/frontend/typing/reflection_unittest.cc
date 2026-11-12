#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "compiler/frontend/builtins/reflection_builtins.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/lexer.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/typing/type_checker.h"

namespace vela::frontend {
namespace {

TEST(ReflectionStandardEnvironmentTest, BootstrapNamesMatchEveryStandardDeclaration) {
  auto tokens = Lexer(kReflectionStandardSource).Tokenize();
  ASSERT_TRUE(tokens.diagnostics.empty()) << DiagnosticsText(tokens.diagnostics);
  const auto parsed = Parser(tokens.tokens, SyntaxContext::Standard).Parse();
  ASSERT_TRUE(parsed.diagnostics.empty()) << DiagnosticsText(parsed.diagnostics);
  EXPECT_EQ(parsed.program.classes.size() + parsed.program.enums.size(),
            kReflectionStandardNames.size());
  EXPECT_TRUE(std::ranges::is_sorted(kReflectionStandardNames));
  for (const auto& owner : parsed.program.classes) {
    EXPECT_TRUE(IsReflectionStandardName(owner.name)) << owner.name;
  }
  for (const auto& owner : parsed.program.enums) {
    EXPECT_TRUE(IsReflectionStandardName(owner.name)) << owner.name;
  }
  EXPECT_FALSE(IsReflectionStandardName("NoSuchReflectionType"));
}

ReflectionSelection Select(std::string_view type) {
  return {
      .module = "models.vela",
      .type = *ParseTypeSource(type),
      .members = {},
      .source_path = "vela.json",
      .location = {.line = 3, .column = 1},
      .source = ParseSourceSnapshot("\n\n{}").source,
  };
}

TEST(ReflectionReceiverTest, PreservesReceiverTypeAcrossQueriesAndAccessors) {
  const SourceInput input{
      .path = "main.vela",
      .text = R"(
[Reflect]
class Counter {
  value: int = 0;
  get amount(): int { return this.value; }
  set amount(value: int): void { this.value = value; }
  add(value: int): int { return this.value + value; }
  static total: int = 0;
  static reset(): void { Counter.total = 0; }
}
func forward<T>(): TypeInfo<T> { return Reflect.typeInfo<T>(); }
func main(): void {
  const counter = Counter();
  const info: TypeInfo<Counter> = forward<Counter>();
  const fields: FieldInfo<Counter>[] = info.getFields();
  const field: FieldInfo<Counter>? = info.findField('value');
  if (field != null) { field.setValue(counter, 20); }
  const property: PropertyInfo<Counter>? = info.findProperty('amount');
  if (property != null) {
    property.setValue(counter, 30);
    const getter: MethodInfo<Counter>? = property.getter;
    if (getter != null) { getter.invoke(counter, CallArguments()); }
  }
  const method: MethodInfo<Counter>? = info.findMethod('add');
  if (method != null) { method.invoke(counter, CallArguments(items: [CallArgument(value: 1)])); }
  const erased: TypeInfo<dynamic> = Reflect.typeOf(counter);
  for (const item in erased.getFields()) { item.getValue(counter); }
  const total: StaticFieldInfo? = info.findStaticField('total');
  if (total != null) { total.setValue(1); total.getValue(); }
  const reset: StaticMethodInfo? = info.findStaticMethod('reset');
  if (reset != null) { reset.invoke(CallArguments()); }
}
)",
  };
  auto parsed = LoadSources(std::span(&input, 1), input.path);
  ASSERT_TRUE(parsed.diagnostics.empty()) << DiagnosticsText(parsed.diagnostics);
  const auto checked = TypeChecker().Check(parsed.program);
  EXPECT_TRUE(checked.Succeeded()) << DiagnosticsText(checked.diagnostics);
}

TEST(ReflectionReceiverTest, RejectsWrongKnownReceiversAndReceiverArgumentsForStatics) {
  for (const std::string_view operation : {
           "const field = info.findField('value'); if (field != null) { field.getValue('wrong'); }",
           "const field = info.findField('value'); if (field != null) { field.setValue('wrong', "
           "1); }",
           "const method = info.findMethod('add'); if (method != null) { method.invoke('wrong', "
           "CallArguments()); }",
           "const property = info.findProperty('amount'); if (property != null) { "
           "property.setValue('wrong', 1); }",
           "const property = info.findProperty('amount'); if (property != null) { const getter = "
           "property.getter; if (getter != null) { getter.invoke('wrong', CallArguments()); } }",
           "const field = info.findStaticField('total'); if (field != null) { "
           "field.getValue(Counter()); }",
           "const field = info.findStaticField('total'); if (field != null) { "
           "field.setValue(Counter(), 1); }",
           "const method = info.findStaticMethod('reset'); if (method != null) { "
           "method.invoke(Counter(), CallArguments()); }",
       }) {
    SCOPED_TRACE(operation);
    const std::string source =
        "class Counter { value: int = 0; static total: int = 0; "
        "get amount(): int { return this.value; } set amount(value: int): void {} "
        "add(value: int): int { return value; } static reset(): void {} } "
        "func main(): void { const info = Reflect.typeInfo<Counter>(); " +
        std::string(operation) + " }";
    const SourceInput input{.path = "main.vela", .text = source};
    auto parsed = LoadSources(std::span(&input, 1), input.path);
    ASSERT_TRUE(parsed.diagnostics.empty()) << DiagnosticsText(parsed.diagnostics);
    const auto checked = TypeChecker().Check(parsed.program);
    EXPECT_FALSE(checked.Succeeded());
  }
}

TEST(ReflectionAttributeTest, BindsClassMembersAndRepeatedDerivedAttributesFromSource) {
  const SourceInput input{
      .path = "main.vela",
      .text = R"(
[AttributeUsage(allowMultiple: true)]
class Button extends Reflect { label: string = ''; }
[Reflect]
class Whole { value: int = 0; other: int = 1; }
class Partial {
  value: int = 0;
  [Button(label: 'first')]
  [Button(label: 'second')]
  reset(): void { this.value = 0; }
}
func main(): void { const info = Reflect.typeInfo<Whole>(); }
)",
  };
  auto parsed = LoadSources(std::span(&input, 1), input.path);
  ASSERT_TRUE(parsed.diagnostics.empty()) << DiagnosticsText(parsed.diagnostics);
  const auto checked = TypeChecker().Check(parsed.program);
  ASSERT_TRUE(checked.Succeeded()) << DiagnosticsText(checked.diagnostics);
  const auto& registrations = checked.model.reflection;
  const auto whole = std::ranges::find_if(
      registrations, [](const auto& registration) { return registration.type.name == "Whole"; });
  ASSERT_NE(whole, registrations.end());
  EXPECT_EQ(whole->members.size(), 3U);
  EXPECT_EQ(
      std::ranges::count_if(checked.model.attributes,
                            [](const auto& attribute) { return attribute.type.name == "Button"; }),
      2);
}

TEST(ReflectionAttributeTest, RejectsRepeatedSingletonsAndComputedData) {
  for (const std::string_view declaration : {
           "[Reflect] [Reflect] class Box {}",
           "class Button extends Reflect { label: string; } [Button(label: text())] class Box {} "
           "func text(): string { return 'x'; }",
           "class Button extends Reflect { constructor() {} } [Button] class Box {}",
           "class Button {} [Button] class Box {}",
           "class Button extends Reflect { label: string; } [Button] class Box {}",
           "class Button extends Reflect { value: int?; } [Button] class Box {}",
           "class Button extends Reflect { value: dynamic; } [Button] class Box {}",
           "class Button extends Reflect { label: string = text(); } [Button(label: 'literal')] "
           "class Box {} func text(): string { return 'x'; }",
           "class Button extends Reflect {} [Button(unknown: 1)] class Box {}",
           "class Button extends Reflect { label: string = ''; } [Button(label: 'a', label: 'b')] "
           "class Box {}",
           "class Button extends Reflect { label: string = 'a' + 'b'; } [Button] class Box {}",
           "class Button extends Reflect { value: int = -(1 + 2); } [Button] class Box {}",
           "[AttributeUsage] class Plain {}",
           "class Button extends Reflect {} class Box { [AttributeUsage] run(): void {} }",
           "[AttributeUsage(allowMultiple: true)] class Base extends Reflect {} class Child "
           "extends Base {} [Child] [Child] class Box {}",
           "[Reflect] func top<T>(): void {}",
           "[Missing] class Box<T> {}",
           "class Box { [Missing] use<T>(): void {} }",
           "interface Box<T> { [Missing] use(): void; }",
           "func main(): void { const result = Reflect.findMethods<int>(); }",
           "func main(): void { const result = Reflect.findMethods<Reflect>(); result[0].attribute "
           "= Reflect(); }",
           "func main(): void { const info = typeInfo<int>(); }",
       }) {
    const SourceInput input{.path = "main.vela", .text = declaration};
    auto parsed = LoadSources(std::span(&input, 1), input.path);
    ASSERT_TRUE(parsed.diagnostics.empty()) << DiagnosticsText(parsed.diagnostics);
    const auto checked = TypeChecker().Check(parsed.program);
    EXPECT_FALSE(checked.Succeeded()) << declaration;
  }
}

TEST(ReflectionAttributeTest, ChecksFunctionAndInterfaceTemplateAttributesOnce) {
  for (const std::string_view source : {
           "[Reflect] func top(): void {}",
           "[Reflect] func top<T>(): void {}",
           "interface Box { [Reflect] [Reflect] use<T>(): void; }",
           "interface Box<T> { [Reflect] [Reflect] use<U>(): void; }",
       }) {
    SCOPED_TRACE(source);
    const SourceInput input{.path = "main.vela", .text = source};
    auto parsed = LoadSources(std::span(&input, 1), input.path);
    ASSERT_TRUE(parsed.diagnostics.empty()) << DiagnosticsText(parsed.diagnostics);
    const auto checked = TypeChecker().Check(parsed.program);
    EXPECT_FALSE(checked.Succeeded());
    EXPECT_EQ(checked.diagnostics.size(), 1U) << DiagnosticsText(checked.diagnostics);
  }
}

TEST(ReflectionAttributeTest, ReportsInvalidDefaultsOnceEvenWhenArgumentsOverrideThem) {
  for (const std::string_view argument : {"", "value: 1"}) {
    const std::string source = "class Tag extends Attribute { value: int = 'wrong'; } [Tag(" +
                               std::string(argument) + ")] class Box {}";
    const SourceInput input{.path = "main.vela", .text = source};
    auto parsed = LoadSources(std::span(&input, 1), input.path);
    ASSERT_TRUE(parsed.diagnostics.empty()) << DiagnosticsText(parsed.diagnostics);
    const auto checked = TypeChecker().Check(parsed.program);
    ASSERT_FALSE(checked.Succeeded());
    ASSERT_EQ(checked.diagnostics.size(), 1U) << DiagnosticsText(checked.diagnostics);
    EXPECT_EQ(checked.diagnostics.front().message,
              "field initializer does not match its declared type");
  }
}

TEST(ReflectionRegistrationTest, LoadsAndSpecializesOnlyExplicitlySelectedDeclarations) {
  const std::array sources{
      SourceInput{.path = "main.vela", .text = "func main(): void {}"},
      SourceInput{
          .path = "models.vela",
          .text = R"(
class Box<T> {
  value: T;
  private secret: int = 7;
  echo<U>(value: U): U { return value; }
}
)",
      },
  };
  auto selection = Select("Box<int>");
  selection.members = {
      {.kind = ReflectionMemberKind::Field, .name = "secret", .type_arguments = {}, .location = {}},
      {
          .kind = ReflectionMemberKind::Method,
          .name = "echo",
          .type_arguments = {*ParseTypeSource("string")},
          .location = {},
      },
  };
  auto parsed = LoadSources(sources, "main.vela", std::span(&selection, 1));
  ASSERT_TRUE(parsed.diagnostics.empty());
  const auto checked = TypeChecker().Check(parsed.program);
  ASSERT_TRUE(checked.Succeeded()) << DiagnosticsText(checked.diagnostics);
  const auto& reflected = checked.model.reflection;
  ASSERT_EQ(reflected.size(), 1U);
  EXPECT_EQ(reflected.front().type.name, "$module1.Box<int>");
  const auto& members = reflected.front().members;
  ASSERT_EQ(members.size(), 2U);
  EXPECT_EQ(members.front().name, "secret");
  EXPECT_EQ(members.back().name, "echo");
  ASSERT_EQ(members.back().type_arguments.size(), 1U);
  EXPECT_EQ(members.back().type_arguments.front().name, "string");
  EXPECT_TRUE(checked.model.classes.at("$module1.Box<int>").methods.contains("echo<string>"));
}

TEST(ReflectionRegistrationTest, BindsTypeArgumentsInTheSelectedModulesImportScope) {
  const std::array sources{
      SourceInput{.path = "main.vela", .text = "func main(): void {}"},
      SourceInput{
          .path = "models.vela",
          .text = "import 'values' as values; class Box<T> { value: T; }",
      },
      SourceInput{.path = "values.vela", .text = "export class Value {}"},
  };
  auto selection = Select("Box<values.Value>");
  auto parsed = LoadSources(sources, "main.vela", std::span(&selection, 1));
  ASSERT_TRUE(parsed.diagnostics.empty());
  const auto checked = TypeChecker().Check(parsed.program);
  ASSERT_TRUE(checked.Succeeded());
  ASSERT_EQ(checked.model.reflection.size(), 1U);
  EXPECT_TRUE(checked.model.reflection.front().members.empty());
  EXPECT_EQ(checked.model.reflection.front().type.name, "$module1.Box<$module2.Value>");
}

TEST(ReflectionRegistrationTest, RejectsWrongKindsInheritedSelectionsAndGenericConstraints) {
  const std::array sources{
      SourceInput{.path = "main.vela", .text = "func main(): void {}"},
      SourceInput{
          .path = "models.vela",
          .text = R"(
class Base { inherited: int = 1; }
class Item extends Base {
  value: int = 0;
  convert<T extends int>(value: T): T { return value; }
}
)",
      },
  };
  for (auto member : {
           ReflectionMemberSelection{
               .kind = ReflectionMemberKind::Field,
               .name = "missing",
               .type_arguments = {},
               .location = {},
           },
           ReflectionMemberSelection{
               .kind = ReflectionMemberKind::Method,
               .name = "value",
               .type_arguments = {},
               .location = {},
           },
           ReflectionMemberSelection{
               .kind = ReflectionMemberKind::Field,
               .name = "inherited",
               .type_arguments = {},
               .location = {},
           },
           ReflectionMemberSelection{
               .kind = ReflectionMemberKind::Method,
               .name = "convert",
               .type_arguments = {},
               .location = {},
           },
           ReflectionMemberSelection{
               .kind = ReflectionMemberKind::Method,
               .name = "convert",
               .type_arguments = {*ParseTypeSource("string")},
               .location = {},
           },
       }) {
    auto selection = Select("Item");
    selection.members.push_back(std::move(member));
    auto parsed = LoadSources(sources, "main.vela", std::span(&selection, 1));
    ASSERT_TRUE(parsed.diagnostics.empty());
    const auto checked = TypeChecker().Check(parsed.program);
    ASSERT_FALSE(checked.Succeeded());
    EXPECT_TRUE(std::ranges::any_of(checked.diagnostics, [](const auto& diagnostic) {
      return diagnostic.source_path == "vela.json";
    }));
  }
}

TEST(ReflectionRegistrationTest, RejectsOpenNullableAndNonNominalTypeRegistrations) {
  const SourceInput source{.path = "models.vela", .text = "class Box<T> { value: T; }"};
  for (const std::string_view type : {"Box", "Box<int>?", "int", "Box<int>[]"}) {
    auto selection = Select(type);
    auto parsed = LoadSources(std::span(&source, 1), "models.vela", std::span(&selection, 1));
    ASSERT_TRUE(parsed.diagnostics.empty());
    EXPECT_FALSE(TypeChecker().Check(parsed.program).Succeeded()) << type;
  }
}

TEST(ReflectionRegistrationTest, LoadsExplicitStandardModulesForRegistration) {
  const SourceInput source{.path = "main.vela", .text = "func main(): void {}"};
  auto selection = Select("Math");
  selection.module = "builtin:Math";
  selection.members = {
      {.kind = ReflectionMemberKind::Method, .name = "sqrt", .type_arguments = {}, .location = {}},
  };
  auto parsed = LoadSources(std::span(&source, 1), source.path, std::span(&selection, 1));
  ASSERT_TRUE(parsed.diagnostics.empty());
  const auto checked = TypeChecker().Check(parsed.program);
  ASSERT_TRUE(checked.Succeeded());
  ASSERT_EQ(checked.model.reflection.size(), 1U);
  EXPECT_EQ(checked.model.reflection.front().type.name, "Math");
  EXPECT_EQ(checked.model.reflection.front().members.front().name, "sqrt");
}

}  // namespace
}  // namespace vela::frontend
