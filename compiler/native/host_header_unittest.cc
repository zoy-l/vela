#include "compiler/native/host_header.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "base/layout_description.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::compiler::native {
namespace {

TEST(HostHeaderTest, NominalContractsIgnoreImportOrderAndIncludeConcreteGenericArguments) {
  std::optional<std::string> previous;
  for (const bool reversed : {false, true}) {
    const std::string main =
        (reversed ? "import 'other.vela' as extra; import 'library.vela' as api; "
                  : "import 'library.vela' as api; import 'other.vela' as extra; ") +
        std::string("export func echo(value: api.Box<int>): api.Box<int> { return value; }");
    const std::array sources{
        frontend::SourceInput{.path = "main.vela", .text = main},
        frontend::SourceInput{
            .path = "library.vela",
            .text = R"(
export class Box<T> { value: T;  #hidden: int = 0; }
export interface View<T> { read(): T; }
)",
        },
        frontend::SourceInput{.path = "other.vela", .text = "export class Box<T> { value: T; }"},
    };
    const auto prepared = ir::PrepareParsed(frontend::LoadSources(sources, "main.vela"));
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto& program = prepared.program;
    const auto function = std::ranges::find(program.functions, "echo", &ir::Function::name);
    ASSERT_NE(function, program.functions.end());
    const auto& signature = function->signature;
    ASSERT_TRUE(signature) << "expected the exported echo signature";
    const auto parameter = program.layouts.types.at(signature->parameters.front().type);
    const auto key = layout::TypeKey(program.layouts, parameter);
    if (previous) {
      EXPECT_EQ(key, *previous);
    }
    previous = key;
    const auto header = EmitHostDeclarations(program, "application");
    ASSERT_TRUE(header) << header.error();
    EXPECT_NE(header->find("kModule = \"library.vela\""), std::string::npos);
    EXPECT_NE(header->find("using Arguments = std::tuple<std::int64_t>"), std::string::npos);
    EXPECT_EQ(header->find("Get_hidden"), std::string::npos);
  }
}

TEST(HostHeaderTest, ExportedStructsKeepTheirKindInNominalHostContracts) {
  const auto prepared = ir::test::Prepare(R"(
export struct Point<T> { x: T; y: T; }
export func echo(value: Point<int>): Point<int> { return value; }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto header = EmitHostDeclarations(prepared.program, "application");
  ASSERT_TRUE(header) << header.error();
  EXPECT_TRUE(header->contains("ScriptTypeKind::Struct"));
  EXPECT_TRUE(header->contains("using Arguments = std::tuple<std::int64_t>"));
  EXPECT_TRUE(header->contains("Entry_echo"));
  EXPECT_TRUE(header->contains("Get_x"));
  EXPECT_TRUE(header->contains("Set_y"));
}

TEST(HostHeaderTest, ReportsPrivateNominalsAtThePublicGenerationBoundary) {
  const auto prepared =
      ir::test::Prepare("class Secret {} export func expose(): Secret { return Secret(); }");
  ASSERT_TRUE(prepared.Succeeded());
  const auto header = EmitHostDeclarations(prepared.program, "application");
  ASSERT_FALSE(header);
  EXPECT_NE(header.error().find("not exported"), std::string::npos);
}

TEST(HostHeaderTest, PrivateGenericApplicationsRemainInternalToPublicEntryBodies) {
  const auto prepared = ir::test::Prepare(R"(
class Props { value: int = 7; }
export class Component<P> { props: P; }
export interface Reader<P> { read(): P; }
class Control extends Component<Props> implements Reader<Props> {
  constructor() { super(props: Props()); }
  read(): Props { return this.props; }
}
export func run(): int { return Control().read().value; }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto header = EmitHostDeclarations(prepared.program, "application");
  ASSERT_TRUE(header) << header.error();
  EXPECT_NE(header->find("Entry_run"), std::string::npos);
  EXPECT_EQ(header->find("Type_Component"), std::string::npos);
  EXPECT_EQ(header->find("Type_Reader"), std::string::npos);
}

TEST(HostHeaderTest, RejectsPrivateGenericDependenciesAndInvalidCppNamespaces) {
  const auto prepared = ir::test::Prepare(
      "class Secret {} export class Box<T> { value: T; } "
      "export func expose(): Box<Secret> { return Box(value: Secret()); }");
  ASSERT_TRUE(prepared.Succeeded());
  const auto header = EmitHostDeclarations(prepared.program, "application");
  ASSERT_FALSE(header);
  EXPECT_NE(header.error().find("argument[0]"), std::string::npos);
  const auto invalid_namespace = EmitHostDeclarations(prepared.program, "namespace");
  ASSERT_FALSE(invalid_namespace);
  EXPECT_TRUE(invalid_namespace.error().contains("non-reserved C++ identifiers"));
}

}  // namespace
}  // namespace vela::compiler::native
