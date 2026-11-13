#pragma once

#include <array>
#include <string_view>

#include "base/reflection.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

inline constexpr std::string_view kReflectionStandardSource = R"(
/// Base type for declaration attributes.
class Attribute {}
/// Configures whether the same attribute type may appear repeatedly on one declaration.
class AttributeUsage extends Attribute { allowMultiple: bool = false; }
/// Marks declarations for reflection and supplies the current reflection context.
class Reflect extends Attribute {
  /// Describes a concrete type and preserves T for instance receivers without registering members.
  static external func typeInfo<T>(): TypeInfo<T>;
  /// Describes the actual runtime type without claiming a static receiver type.
  static external func typeOf(value: dynamic): TypeInfo<dynamic>;
  /// Describes the absence of a return value; void is not a generic receiver value type.
  static external func voidType(): TypeInfo<dynamic>;
  /// Returns the registered concrete nominal types in a canonical module URI.
  static external func getTypes(module: string): TypeInfo<dynamic>[];
  /// Finds a registered type without loading code or creating a specialization.
  static external func findType(module: string, name: string, typeArguments: TypeDescriptor[]): TypeInfo<dynamic>?;
  /// Returns the current publication revision.
  static external func revision(): int;
  /// Finds registered public methods directly annotated with T or a derived attribute.
  static external func findMethods<T extends Attribute>(module: string?): MethodAttributeMatch<T>[];
}
/// A method's metadata, copied annotation data and separate instance or static operation.
class MethodAttributeMatch<T extends Attribute> {
  private data: dynamic = null;
  private constructor() {}
  get method(): MemberInfo;
  get attribute(): T;
  get instanceMethod(): MethodInfo<dynamic>?;
  get staticMethod(): StaticMethodInfo?;
}

enum TypeKind {
  Void, Null, Dynamic, Bool, Int, Double, String, MutableString, Array, Set, Map, Function,
  Class, Interface, Enum, Future, Completer, Timer, CancellationToken, ByteArray, ByteData,
  RegExp, RegExpMatch
}
enum MemberKind { Field, Property, Method, Getter, Setter, Constructor, EnumMember }
enum MemberVisibility { Public, Private }
enum MemberScope { Declared, IncludeInherited }
enum MemberVisibilityFilter { Public, Private, All }

/// Queries narrow registration and never grant access permissions.
class MemberQuery {
  scope: MemberScope = MemberScope.IncludeInherited;
  visibility: MemberVisibilityFilter = MemberVisibilityFilter.Public;
}
/// An ordered argument; a null name denotes a positional argument.
class CallArgument { value: dynamic; name: string? = null; }
/// A stable ordered argument snapshot is read when invoke starts.
class CallArguments { items: CallArgument[] = []; }
/// An opaque canonical type identity, comparable and usable as a collection key.
class TypeId { private data: dynamic = null; private constructor() {} }
/// An opaque stable member identity, comparable and usable as a collection key.
class MemberId { private data: dynamic = null; private constructor() {} }

/// Common metadata accepts descriptors of different statically known types.
class TypeDescriptor {
  private data: dynamic = null;
  private constructor() {}
  get id(): TypeId;
  get name(): string;
  get module(): string?;
  get kind(): TypeKind;
  get isNullable(): bool;
  get isRegistered(): bool;
  get typeArguments(): TypeInfo<dynamic>[];
  get baseType(): TypeInfo<dynamic>?;
  get interfaces(): TypeInfo<dynamic>[];
  get signature(): SignatureInfo?;
  isInstance(value: dynamic): bool;
  isAssignableFrom(other: TypeDescriptor): bool;
}
/// T constrains instance receivers; runtime-selected member values retain their actual types.
class TypeInfo<T> extends TypeDescriptor {
  private constructor() {}
  getFields(query: MemberQuery): FieldInfo<T>[];
  findField(name: string, query: MemberQuery): FieldInfo<T>?;
  getProperties(query: MemberQuery): PropertyInfo<T>[];
  findProperty(name: string, query: MemberQuery): PropertyInfo<T>?;
  getMethods(query: MemberQuery): MethodInfo<T>[];
  findMethod(name: string, query: MemberQuery, typeArguments: TypeDescriptor[]): MethodInfo<T>?;
  getStaticFields(query: MemberQuery): StaticFieldInfo[];
  findStaticField(name: string, query: MemberQuery): StaticFieldInfo?;
  getStaticProperties(query: MemberQuery): StaticPropertyInfo[];
  findStaticProperty(name: string, query: MemberQuery): StaticPropertyInfo?;
  getStaticMethods(query: MemberQuery): StaticMethodInfo[];
  findStaticMethod(name: string, query: MemberQuery, typeArguments: TypeDescriptor[]): StaticMethodInfo?;
  getConstructor(): ConstructorInfo?;
  getEnumMembers(): EnumMemberInfo[];
  findEnumMember(name: string): EnumMemberInfo?;
}
/// A member descriptor resolves its stable declaration in the current publication.
class MemberInfo {
  private data: dynamic = null;
  private constructor() {}
  get id(): MemberId;
  get name(): string;
  get declaringType(): TypeInfo<dynamic>;
  get kind(): MemberKind;
  get visibility(): MemberVisibility;
  get isStatic(): bool;
}
class FieldInfo<T> extends MemberInfo {
  private constructor() {}
  get type(): TypeInfo<dynamic>;
  getValue(receiver: T): dynamic;
  setValue(receiver: T, value: dynamic): void;
}
class StaticFieldInfo extends MemberInfo {
  private constructor() {}
  get type(): TypeInfo<dynamic>;
  getValue(): dynamic;
  setValue(value: dynamic): void;
}
class PropertyInfo<T> extends MemberInfo {
  private constructor() {}
  get type(): TypeInfo<dynamic>;
  get canRead(): bool;
  get canWrite(): bool;
  get getter(): MethodInfo<T>?;
  get setter(): MethodInfo<T>?;
  getValue(receiver: T): dynamic;
  setValue(receiver: T, value: dynamic): void;
}
class StaticPropertyInfo extends MemberInfo {
  private constructor() {}
  get type(): TypeInfo<dynamic>;
  get canRead(): bool;
  get canWrite(): bool;
  get getter(): StaticMethodInfo?;
  get setter(): StaticMethodInfo?;
  getValue(): dynamic;
  setValue(value: dynamic): void;
}
class MethodInfo<T> extends MemberInfo {
  private constructor() {}
  get parameters(): ParameterInfo[];
  get returnType(): TypeInfo<dynamic>;
  get typeArguments(): TypeInfo<dynamic>[];
  get isAsync(): bool;
  invoke(receiver: T, arguments: CallArguments): dynamic;
}
class StaticMethodInfo extends MemberInfo {
  private constructor() {}
  get parameters(): ParameterInfo[];
  get returnType(): TypeInfo<dynamic>;
  get typeArguments(): TypeInfo<dynamic>[];
  get isAsync(): bool;
  invoke(arguments: CallArguments): dynamic;
}
class ConstructorInfo extends MemberInfo {
  private constructor() {}
  get parameters(): ParameterInfo[];
  invoke(arguments: CallArguments): dynamic;
}
class EnumMemberInfo extends MemberInfo {
  private constructor() {}
  get ordinal(): int;
  get value(): dynamic;
}
/// A copied logical declaration parameter; defaults are described without evaluation.
class ParameterInfo {
  private data: dynamic = null;
  private constructor() {}
  get name(): string;
  get position(): int;
  get type(): TypeInfo<dynamic>;
  get isOptional(): bool;
  get isRest(): bool;
  get hasDefault(): bool;
}
/// A structural function signature has no invented declaration parameter names.
class SignatureInfo {
  private data: dynamic = null;
  private constructor() {}
  get parameters(): SignatureParameterInfo[];
  get returnType(): TypeInfo<dynamic>;
}
class SignatureParameterInfo {
  private data: dynamic = null;
  private constructor() {}
  get type(): TypeInfo<dynamic>;
  get isOptional(): bool;
  get isRest(): bool;
}
)";

inline constexpr auto kReflectionStandardNames = std::to_array<std::string_view>({
    "Attribute",
    "AttributeUsage",
    reflection::kCallArgumentName,
    reflection::kCallArgumentsName,
    "ConstructorInfo",
    "EnumMemberInfo",
    "FieldInfo",
    "MemberId",
    "MemberInfo",
    "MemberKind",
    "MemberQuery",
    "MemberScope",
    "MemberVisibility",
    "MemberVisibilityFilter",
    "MethodAttributeMatch",
    "MethodInfo",
    "ParameterInfo",
    "PropertyInfo",
    "Reflect",
    "SignatureInfo",
    "SignatureParameterInfo",
    "StaticFieldInfo",
    "StaticMethodInfo",
    "StaticPropertyInfo",
    "TypeDescriptor",
    "TypeId",
    "TypeInfo",
    "TypeKind",
});

[[nodiscard]] bool IsReflectionStandardName(std::string_view name) noexcept;
[[nodiscard]] Diagnostics BindReflectionDeclarations(Program& program);

}  // namespace vela::frontend
