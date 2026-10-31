#include <string_view>
#include <tuple>

#include "gtest/gtest.h"

#include "compiler/frontend/checker_test_support.h"

namespace vela::frontend {
namespace {

class GenericDefinitionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(GenericDefinitionTest, ChecksUnusedDefinitionsByTheirDeclaredCapabilities) {
  const auto result = CheckTestSource(GetParam());
  for (const auto& diagnostic : result.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ':' << diagnostic.location.column << ": "
                  << diagnostic.message;
  }
  EXPECT_TRUE(result.Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    StaticContracts,
    GenericDefinitionTest,
    testing::Values(
        "func identity<T>(value:T):T{return value;}",
        "interface Reader<E>{read():E;} "
        "func read<T extends Reader<E>,E>(value:T):E{return value.read();}",
        "interface Comparable<T>{compareTo(other:T):int;} "
        "func compare<T extends Comparable<T>>(a:T,b:T):int{return a.compareTo(b);}",
        "func increment<T extends int>(value:T):int{return value+1;}",
        "func size<T extends string>(value:T):int{return value.length;}",
        "func get<T extends int[]>(value:T):int{return value[0];}",
        "func apply<F extends (int)=>int>(operation:F):int{return operation(1);}",
        "func forward<U extends T,T>(value:U):T{return value;}",
        "func same<T extends int>(left:T,right:T):bool{return left==right;}",
        "func format<T extends int>(value:T):string{return '${value}';}",
        "func joined<T extends string>(values:T[]):string{return values.join(',');}",
        "func sorted<T extends string>(values:T[]):void{values.sort();}",
        "func absent<T>(value:T):bool{return value==null;}",
        "class Base{} func narrow<T extends Base>(value:T):T{"
        "if(value is Base){return value;} return value;}",
        "interface Comparable<T>{compareTo(other:T):int;} "
        "class Ordered<T extends Comparable<T>>{value:T;}",
        "interface Reader<T>{read():T;} type Bound<T extends Reader<int>> = T[]; "
        "type Forward<T extends Reader<int>> = Bound<T>;",
        "interface Reader<T>{read():T;} "
        "func read<T extends Reader<int>?>(value:T):int{"
        "if(value!=null){return value.read();} return 0;}",
        "interface Mapper{map<T extends int>(value:T):T;} "
        "class Service implements Mapper{map<U extends int>(value:U):U{return value;}}",
        "class Base{map<T extends int>(value:T):T{return value;}} "
        "class Derived extends Base{override map<U extends int>(value:U):U{return value;}}"));

using InvalidContract = std::tuple<std::string_view, std::string_view>;
class GenericContractErrorTest : public testing::TestWithParam<InvalidContract> {};

TEST_P(GenericContractErrorTest, DiagnosesTheDeclarationOrApplicationBoundary) {
  const auto& [source, message] = GetParam();
  const auto result = CheckTestSource(source);
  EXPECT_FALSE(result.Succeeded());
  bool found = false;
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.message.contains(message)) {
      found = true;
      EXPECT_GT(diagnostic.location.line, 0U);
      EXPECT_GT(diagnostic.location.column, 0U);
    }
  }
  EXPECT_TRUE(found) << "missing diagnostic: " << message;
}

INSTANTIATE_TEST_SUITE_P(
    StaticContracts,
    GenericContractErrorTest,
    testing::Values(
        InvalidContract{"func bad<T>(value:T):int{return value.length;}", "needs an 'extends'"},
        InvalidContract{"func bad<T>(a:T,b:T):bool{return a==b;}", "needs an 'extends'"},
        InvalidContract{"func bad<T extends int,U extends int>(value:T):U{return value;}",
                        "cannot return"},
        InvalidContract{"class Base{} func bad<T extends Base>():T{return T();}", "is not a value"},
        InvalidContract{"func bad<T extends int>(value:T):T{return 1;}", "cannot return"},
        InvalidContract{"func bad<T extends T>(value:T):T{return value;}", "cyclic bound"},
        InvalidContract{"func bad<T extends U,U extends T>(value:T):T{return value;}",
                        "cyclic bound"},
        InvalidContract{"type Id<T> = T; func bad<T extends Id<T>>(value:T):T{return value;}",
                        "cyclic bound"},
        InvalidContract{"type Maybe<T> = T?; "
                        "func bad<T extends Maybe<U>,U extends T>():void{}",
                        "cyclic bound"},
        InvalidContract{"func bad<T extends dynamic>(value:T):T{return value;}",
                        "dynamic cannot be a generic bound"},
        InvalidContract{"func bad<T extends void>():void{}", "generic bound must be a value type"},
        InvalidContract{"class Bad<T>{value:T; constructor(value:T){}}", "must be initialized"},
        InvalidContract{"interface Reader<T>{read():T;} "
                        "type Bound<T extends Reader<int>> = T[]; type Bad<T> = Bound<T>;",
                        "does not satisfy the bound"},
        InvalidContract{"interface Reader<T>{read():T;} "
                        "func bad<T extends Reader<int>?>(value:T):int{return value.read();}",
                        "nullable receiver"},
        InvalidContract{"func bounded<T extends int>(value:T):T{return value;} "
                        "func use(value:dynamic):void{bounded<dynamic>(value);}",
                        "does not satisfy the bound"},
        InvalidContract{"func bounded<T extends int>(value:T):T{return value;} "
                        "func use(value:int?):void{bounded<int?>(value);}",
                        "does not satisfy the bound"},
        InvalidContract{"interface Reader<T>{read():T;} "
                        "class A implements Reader<int>{read():int{return 1;}} "
                        "class B extends A implements Reader<string>{read():string{return '';}}",
                        "conflicting applications"},
        InvalidContract{"interface Mapper{map<T>(value:T):T;} class Bad implements Mapper{}",
                        "missing generic interface method"},
        InvalidContract{"interface Mapper{map<T>(value:T):T;} "
                        "class Bad implements Mapper{private map<T>(value:T):T{return value;}}",
                        "generic interface method implementation does not match"},
        InvalidContract{"interface Mapper{map<T>(value:T):T;} "
                        "class Bad implements Mapper{map<T extends int>(value:T):T{return value;}}",
                        "generic interface method implementation does not match"},
        InvalidContract{
            "class Base{map<T>(value:T):T{return value;}} "
            "class Bad extends Base{override map<T extends int>(value:T):T{return value;}}",
            "generic override signature or bounds do not match"}));

}  // namespace
}  // namespace vela::frontend
