#include <string_view>
#include <tuple>

#include "gtest/gtest.h"

#include "compiler/frontend/typing/checker_test_support.h"

namespace vela::frontend {
namespace {

class BoundedInferenceTest : public testing::TestWithParam<std::string_view> {};

TEST_P(BoundedInferenceTest, UsesTheSameRelationsForEvidenceBoundsAndFinalBindings) {
  const auto result = CheckTestSource(GetParam());
  for (const auto& diagnostic : result.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ':' << diagnostic.location.column << ": "
                  << diagnostic.message;
  }
  EXPECT_TRUE(result.Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    NominalEvidence,
    BoundedInferenceTest,
    testing::Values(
        "interface Reader<E>{read():E;} "
        "class IntReader implements Reader<int>{read():int{return 42;}} "
        "func read<T extends Reader<E>,E>(source:T):E{return source.read();} "
        "func main():int{return read(IntReader());}",
        "interface Reader<E>{read():E;} "
        "class IntReader implements Reader<int>{read():int{return 42;}} "
        "func read<E>(source:Reader<E>):E{return source.read();} "
        "func main():int{return read(IntReader());}",
        "class Base<T>{value:T;} "
        "class Derived extends Base<int>{constructor(){super(value:42);}} "
        "func read<T>(source:Base<T>):T{return source.value;} "
        "func main():int{return read(Derived());}",
        "class Base<T>{} class Derived<T> extends Base<T>{} "
        "func make<T>():Derived<T>{return Derived<T>();} "
        "func main():void{const value:Base<int> = make();}",
        "class Base{} class Derived extends Base{} "
        "func widen<T,U extends T>(value:U):T{return value;} "
        "func main():void{const value:Base = widen(Derived());}",
        "class A<T>{read():int{return 42;}} class B extends A<B>{} class C extends B{} "
        "func read<X extends A<X>>(value:X):int{return value.read();} "
        "func main():int{return read(C());}",
        "interface Reader<E>{read():E;} "
        "class IntReader implements Reader<int>{read():int{return 42;}} "
        "func map<T extends Reader<E>,E,U>(operation:(E)=>U,source:T):U{"
        "return operation(source.read());} "
        "func main():int{return map(value=>value+1,IntReader());}",
        "interface Reader<E>{read():E;} "
        "class IntReader implements Reader<int>{read():int{return 42;}} "
        "func read<T extends Reader<E>?,E>(source:T,fallback:E):E{"
        "if(source!=null){return source.read();} return fallback;} "
        "func main():int{const source:IntReader? = IntReader();return read(source,0);}",
        "interface Reader<E>{read():E;} "
        "class IntReader implements Reader<int>{read():int{return 42;}} "
        "type ReaderOp<T extends Reader<E>,E> = (T)=>E; "
        "func read<T extends Reader<E>,E>(source:T):E{return source.read();} "
        "func main():int{const operation:ReaderOp<IntReader,int> = read;return "
        "operation(IntReader());}",
        "interface Reader<E>{read():E;} "
        "class IntReader implements Reader<int>{read():int{return 42;}} "
        "func read<T extends Reader<E>,E>(source:T):E{return source.read();} "
        "func identity<T>(value:T):T{return value;} "
        "func main():int{return identity(read(IntReader()));}",
        "func select<T extends int[]>(value:T):T{return value;} "
        "func main():int{return select([42])[0];}",
        "func head<T extends E[],E>(source:T):E{return source[0];} "
        "func forward<P extends int[]>(source:P):int{return head(source);} "
        "func main():int{return forward([42]);}",
        "type Items<E> = E[]; "
        "func head<T extends Items<E>,E>(source:T):E{return source[0];} "
        "func forward<P extends int[]>(source:P):int{return head(source);} "
        "func main():int{return forward([42]);}",
        "func apply<F extends (T)=>U,T,U>(operation:F,value:T):U{return operation(value);} "
        "func forward<P extends (int)=>int>(operation:P):int{return apply(operation,41);} "
        "func increment(value:int):int{return value+1;} "
        "func main():int{return forward(increment);}",
        "interface Reader<E>{read():E;} "
        "func read<E>(source:Reader<E>):E{return source.read();} "
        "func forward<P extends Reader<int>>(source:P):int{return read(source);}",
        "func forward<T extends Future<dynamic>>(source:T):Future<dynamic>{return source;} "
        "func main():void{const value = forward(Future.value(42));}",
        "interface Next<T>{next():T;} "
        "class Left implements Next<Right>{next():Right{return Right();}} "
        "class Right implements Next<Left>{next():Left{return Left();}} "
        "func next<T extends Next<U>,U extends Next<T>>(value:T):U{return value.next();} "
        "func main():void{const value:Right = next(Left());}"));

using InvalidInference = std::tuple<std::string_view, std::string_view>;
class BoundedInferenceErrorTest : public testing::TestWithParam<InvalidInference> {};

TEST_P(BoundedInferenceErrorTest, RejectsInsufficientOrContradictoryEvidence) {
  const auto& [source, expected] = GetParam();
  const auto result = CheckTestSource(source);
  EXPECT_FALSE(result.Succeeded());
  bool found = false;
  for (const auto& diagnostic : result.diagnostics) {
    found = found || diagnostic.message.contains(expected);
  }
  EXPECT_TRUE(found) << "missing diagnostic: " << expected;
}

INSTANTIATE_TEST_SUITE_P(
    NominalEvidence,
    BoundedInferenceErrorTest,
    testing::Values(
        InvalidInference{
            "interface Reader<E>{read():E;} "
            "class IntReader implements Reader<int>{read():int{return 42;}} "
            "func read<T extends Reader<E>,E>(source:T,fallback:E):E{return source.read();} "
            "func main():void{read(IntReader(),'wrong');}",
            "conflicting inferred types"},
        InvalidInference{"class A<T>{} class B extends A<B>{} class C extends B{} "
                         "func read<X extends A<X>>(value:X):void{} "
                         "func main():void{read<C>(C());}",
                         "does not satisfy the bound"},
        InvalidInference{"func empty<T extends int>():T[]{return [];} "
                         "func main():void{empty();}",
                         "cannot infer type argument"},
        InvalidInference{"class Base{} class Left extends Base{} class Right extends Base{} "
                         "func choose<T>(left:T,right:T):T{return left;} "
                         "func main():void{const value:Base = choose(Left(),Right());}",
                         "conflicting inferred types"},
        InvalidInference{
            "class Base{} class Derived extends Base{} "
            "func choose<T>(left:T[],right:T[]):T[]{return left;} "
            "func main():void{const left=[Base()];const right=[Derived()];choose(left,right);}",
            "conflicting inferred types"},
        InvalidInference{"func bounded<T extends int>(value:T):T{return value;} "
                         "func main(value:dynamic):int{return bounded(value);}",
                         "does not satisfy the bound"},
        InvalidInference{"func apply<T>(operation:(T)=>T,value:T):T{return operation(value);} "
                         "func main():int{const operation = (value:dynamic):dynamic=>value; "
                         "return apply<int>(operation,1);}",
                         "call argument type does not match"},
        InvalidInference{"func apply<T>(operation:(int)=>T):T{return operation(1);} "
                         "func main():int{return apply((value:dynamic):int=>42);}",
                         "does not match type evidence"},
        InvalidInference{"func bounded<T extends int[]>(value:T):T{return value;} "
                         "func main():void{bounded([true]);}",
                         "does not satisfy the bound"},
        InvalidInference{"func forward<T extends Future<int>>(source:T):T{return source;} "
                         "func main(source:Future<dynamic>):void{forward(source);}",
                         "does not satisfy the bound"},
        InvalidInference{"interface Reader<E>{read():E;} class Duck{read():int{return 42;}} "
                         "func read<T extends Reader<E>,E>(source:T):E{return source.read();} "
                         "func main():void{read(Duck());}",
                         "does not satisfy the bound"},
        InvalidInference{"func head<T extends E[],E>(source:T):E{return source[0];} "
                         "func forward<P extends int[]?>(source:P):int{return head(source);}",
                         "does not satisfy the bound"},
        InvalidInference{"interface Reader<E>{read():E;} "
                         "class IntReader implements Reader<int>{read():int{return 42;}} "
                         "func read<T extends Reader<E>,E>(source:T):E{return source.read();} "
                         "func main():void{read<IntReader,int?>(IntReader());}",
                         "does not satisfy the bound"}));

}  // namespace
}  // namespace vela::frontend
