#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace vela {

enum class MemberKind : std::uint8_t { Field, Method, Property };

// 编号引用所属程序的类型、类和函数表；Field.type 描述写入时必须满足的完整类型。
struct MemberDefinition final {
  std::uint32_t receiver_type = 0;
  std::string name;
  MemberKind kind = MemberKind::Field;
  // Field 引用字段编号，Method 引用绑定 callable 的类编号，Property 引用 getter 函数编号；
  // 只有 setter 的属性以 kInvalidFunctionTarget 表示缺少 getter。
  std::uint32_t target = 0;
  std::optional<std::uint32_t> type = std::nullopt;
  std::optional<std::uint32_t> setter = std::nullopt;
};

}  // namespace vela
