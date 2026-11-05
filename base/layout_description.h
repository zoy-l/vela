#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/callable.h"

namespace vela::layout {

// 编译产物只保存值形状与表内编号；实例化时才建立 Isolate 内的指针关系。
struct Type final {
  enum class Kind : std::uint8_t {
    Dynamic,
    Boolean,
    Integer,
    Double,
    String,
    MutableString,
    ByteArray,
    ByteData,
    RegExp,
    RegExpMatch,
    Instance,
    Enum,
    Interface,
    Array,
    Collection,
    Callable,
    Future,
    Completer,
    Timer,
    CancellationToken,
  };
  Kind kind = Kind::Dynamic;
  bool nullable = false;
  // kind 决定引用 classes、interfaces、arrays、collections 或 signatures 表。
  std::uint32_t index = 0;
};

struct Array final {
  std::string name;
  Type element;
};

struct Collection final {
  std::string name;
  Type key;
  std::optional<Type> value = std::nullopt;
};

struct Signature final {
  struct Parameter final {
    Type type;
    bool optional = false;
    bool rest = false;
  };
  std::string name;
  std::vector<Parameter> parameters;
  std::optional<Type> result = std::nullopt;
};

struct Class final {
  enum class Kind : std::uint8_t { Instance, RuntimeError, DateParts, Enum, Host };
  std::string name;
  std::string native_key;
  std::string native_contract;
  Kind kind = Kind::Instance;
  std::uint32_t field_count = 0;
  std::vector<std::uint32_t> reference_fields;
  std::optional<CallableTarget> callable;
  std::vector<std::uint32_t> interfaces;
  std::vector<std::uint32_t> dispatch_targets;
  std::optional<std::uint32_t> callable_type = std::nullopt;
};

// 源声明事实供公共头和绑定声明生成使用。原生映像只需要规范化后的名义键。
struct Nominal final {
  std::string module;
  std::string name;
  std::vector<Type> arguments;
  bool exported = false;
};

// arrays/collections 包含嵌套类型；操作数表另外保存 IR 类型编号到描述表的映射。
// 名字用于增量会话的稳定身份，不参与运行时类型形状推断。
struct Program final {
  std::vector<std::string> interfaces;
  std::vector<Class> classes;
  std::vector<Array> arrays;
  std::vector<Collection> collections;
  std::vector<Signature> signatures;
  std::vector<Type> types;
  std::vector<std::uint32_t> array_types;
  std::vector<std::uint32_t> collection_types;
  std::map<std::string, Nominal, std::less<>> nominals;
};

[[nodiscard]] std::string NominalKey(const Program& program, const Nominal& nominal);

// 规范键保留完整结构与名义声明名，不依赖描述表的局部编号或容器调试名称。
[[nodiscard]] std::string TypeKey(const Program& program, const Type& type);
[[nodiscard]] bool IsSameType(const Program& left_program,
                              const Type& left,
                              const Program& right_program,
                              const Type& right);
[[nodiscard]] Type ImportType(Program& target, const Program& source, const Type& type);

}  // namespace vela::layout
