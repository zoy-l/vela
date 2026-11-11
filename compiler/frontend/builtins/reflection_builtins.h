#pragma once

#include <array>
#include <string_view>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

inline constexpr std::string_view kReflectionStandardSource = R"(
/// Base type for declaration attributes.
class Attribute {}
/// Configures whether the same attribute type may appear repeatedly on one declaration.
class AttributeUsage extends Attribute { allowMultiple: bool = false; }
/// Marks declarations for reflection and provides the current reflection context's queries.
class Reflect extends Attribute {
  /// Describes a concrete type without registering its members.
  static external func typeInfo<T>(): TypeInfo;
  /// Describes the actual value's type, including declared container element types.
  static external func typeOf(value: dynamic): TypeInfo;
  /// Returns the registered concrete nominal types in a canonical module URI.
  static external func getTypes(module: string): TypeInfo[];
  /// Finds a registered type without loading code or creating a specialization.
  static external func findType(module: string, name: string, typeArguments: TypeInfo[]): TypeInfo?;
  /// Returns the current publication revision.
  static external func revision(): int;
  /// Finds registered public methods directly annotated with T or a derived attribute.
  static external func findMethods<T extends Attribute>(module: string?): MethodAttributeMatch<T>[];
}

/// A method descriptor paired with an independent copy of its annotation data.
class MethodAttributeMatch<T extends Attribute> {
  private data: dynamic = null;
  private constructor() {}
  get method(): MethodInfo;
  get attribute(): T;
}

enum TypeKind {
  Void, Null, Dynamic, Bool, Int, Double, String, MutableString, Array, Set, Map, Function,
  Class, Interface, Enum, Future, Completer, Timer, CancellationToken, ByteArray, ByteData,
  RegExp, RegExpMatch
}
enum MemberKind { Field, Property, Method, Getter, Setter, Constructor, EnumMember }
enum MemberVisibility { Public, Private }
enum MemberScope { Declared, IncludeInherited }
enum MemberStorageFilter { Instance, Static, All }
enum MemberVisibilityFilter { Public, Private, All }

/// Narrows a registered member directory without granting access permissions.
class MemberQuery {
  scope: MemberScope = MemberScope.IncludeInherited;
  storage: MemberStorageFilter = MemberStorageFilter.All;
  visibility: MemberVisibilityFilter = MemberVisibilityFilter.Public;
}
/// An ordered argument; a null name denotes a positional argument.
class CallArgument {
  value: dynamic;
  name: string? = null;
}
/// A stable ordered argument snapshot is read when invoke starts.
class CallArguments { items: CallArgument[] = []; }

/// An opaque canonical type identity, comparable and usable as a collection key.
class TypeId { private data: dynamic = null; private constructor() {} }
/// An opaque stable member identity, comparable and usable as a collection key.
class MemberId { private data: dynamic = null; private constructor() {} }

/// A current-context type descriptor. Member access requires explicit registration.
class TypeInfo {
  private data: dynamic = null;
  private constructor() {}
  get id(): TypeId;
  get name(): string;
  get module(): string?;
  get kind(): TypeKind;
  get isNullable(): bool;
  get isRegistered(): bool;
  get typeArguments(): TypeInfo[];
  get baseType(): TypeInfo?;
  get interfaces(): TypeInfo[];
  get signature(): SignatureInfo?;
  isInstance(value: dynamic): bool;
  isAssignableFrom(other: TypeInfo): bool;
  getFields(query: MemberQuery): FieldInfo[];
  findField(name: string, query: MemberQuery): FieldInfo?;
  getProperties(query: MemberQuery): PropertyInfo[];
  findProperty(name: string, query: MemberQuery): PropertyInfo?;
  getMethods(query: MemberQuery): MethodInfo[];
  findMethod(name: string, query: MemberQuery, typeArguments: TypeInfo[]): MethodInfo?;
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
  get declaringType(): TypeInfo;
  get kind(): MemberKind;
  get visibility(): MemberVisibility;
  get isStatic(): bool;
}
class FieldInfo extends MemberInfo {
  private constructor() {}
  get type(): TypeInfo;
  getValue(receiver: dynamic): dynamic;
  setValue(receiver: dynamic, value: dynamic): void;
}
class PropertyInfo extends MemberInfo {
  private constructor() {}
  get type(): TypeInfo;
  get canRead(): bool;
  get canWrite(): bool;
  get getter(): MethodInfo?;
  get setter(): MethodInfo?;
  getValue(receiver: dynamic): dynamic;
  setValue(receiver: dynamic, value: dynamic): void;
}
class MethodInfo extends MemberInfo {
  private constructor() {}
  get parameters(): ParameterInfo[];
  get returnType(): TypeInfo;
  get typeArguments(): TypeInfo[];
  get isAsync(): bool;
  invoke(receiver: dynamic, arguments: CallArguments): dynamic;
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
  get type(): TypeInfo;
  get isOptional(): bool;
  get isRest(): bool;
  get hasDefault(): bool;
}
/// A structural function signature has no invented declaration parameter names.
class SignatureInfo {
  private data: dynamic = null;
  private constructor() {}
  get parameters(): SignatureParameterInfo[];
  get returnType(): TypeInfo;
}
class SignatureParameterInfo {
  private data: dynamic = null;
  private constructor() {}
  get type(): TypeInfo;
  get isOptional(): bool;
  get isRest(): bool;
}
)";

inline constexpr auto kReflectionStandardNames = std::to_array<std::string_view>({
    "Attribute",
    "AttributeUsage",
    "CallArgument",
    "CallArguments",
    "ConstructorInfo",
    "EnumMemberInfo",
    "FieldInfo",
    "MemberId",
    "MemberInfo",
    "MemberKind",
    "MemberQuery",
    "MemberScope",
    "MemberStorageFilter",
    "MemberVisibility",
    "MemberVisibilityFilter",
    "MethodAttributeMatch",
    "MethodInfo",
    "ParameterInfo",
    "PropertyInfo",
    "Reflect",
    "SignatureInfo",
    "SignatureParameterInfo",
    "TypeId",
    "TypeInfo",
    "TypeKind",
});

[[nodiscard]] bool IsReflectionStandardName(std::string_view name) noexcept;
[[nodiscard]] Diagnostics BindReflectionDeclarations(Program& program);

}  // namespace vela::frontend
