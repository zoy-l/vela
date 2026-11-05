#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/Support/JSON.h>

#include "base/layout_description.h"
#include "base/native_binding.h"

namespace vela::bindgen {

enum class Ownership : std::uint8_t { Value, Borrow, Unique, Shared, Local };
using MemberKind = NativeMemberKind;

struct Source final {
  std::string path;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

struct Type final {
  layout::Program layouts;
  std::optional<layout::Type> value = std::nullopt;
  Ownership ownership = Ownership::Value;
  std::string cpp;
  bool is_const = false;

  [[nodiscard]] bool IsVoid() const noexcept { return !value; }
  [[nodiscard]] std::string ContractKey() const;
  [[nodiscard]] bool IsSameLogicalType(const Type& other) const;
};

[[nodiscard]] Type HostType(std::string_view key, Ownership ownership);

struct Parameter final {
  std::string name;
  Type type;
  Source source;
};

struct Member final {
  MemberKind kind = MemberKind::Method;
  std::string name;
  std::string key;
  std::string cpp_name;
  std::string cpp_pointer_type;
  std::string documentation;
  std::vector<Parameter> parameters;
  Type result;
  Source source;
  bool context = false;
  bool fallible = false;
  bool is_const = false;
};

struct Class final {
  std::string key;
  std::string name;
  std::string cpp;
  std::string documentation;
  Source source;
  std::vector<Member> members;
};

struct Module final {
  std::string name;
  std::string target;
  std::string clang_version;
  std::string directory;
  std::vector<std::string> arguments;
  std::vector<std::string> inputs;
  std::map<std::string, std::string> input_fingerprints;
  std::vector<Class> classes;
  struct Dependency final {
    std::string module;
    std::string source;
    std::string header;
    std::vector<Class> classes;
  };
  std::vector<Dependency> dependencies;
};

[[nodiscard]] llvm::json::Object Serialize(const Module& module);
[[nodiscard]] std::string PublicContract(const Class& type);
[[nodiscard]] std::string RegistrationNamespace(std::string_view module);

}  // namespace vela::bindgen
