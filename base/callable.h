#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vela {

enum class CallableMode : std::uint8_t { Plain, Closure, BoundMethod, InterfaceMethod };

inline constexpr std::uint32_t kInvalidFunctionTarget = std::numeric_limits<std::uint32_t>::max();

struct CallableTarget final {
  std::uint32_t function = 0;
  CallableMode mode = CallableMode::Plain;
};

// 类型编号引用所属函数的规范类型表；同一契约贯穿 IR、原生映像和动态调用。
struct CallSignature final {
  struct Parameter final {
    std::string name;
    std::uint32_t type = 0;
    bool optional = false;
    bool rest = false;
  };
  std::vector<Parameter> parameters;
  std::optional<std::uint32_t> result = std::nullopt;
  std::uint8_t hidden_parameters = 0;

  void IndexParameters() {
    parameter_name_order.resize(parameters.size());
    std::ranges::iota(parameter_name_order, 0U);
    std::ranges::sort(parameter_name_order, {}, [this](std::uint32_t index) {
      return std::string_view(parameters.at(index).name);
    });
  }

  [[nodiscard]] std::optional<std::uint32_t> ParameterIndex(std::string_view name) const {
    const auto found = std::ranges::lower_bound(
        parameter_name_order, name, {},
        [this](std::uint32_t index) { return std::string_view(parameters.at(index).name); });
    if (found == parameter_name_order.end() || parameters.at(*found).name != name) {
      return std::nullopt;
    }
    return *found;
  }

  // 构造时按名字建立索引，发布为只读元数据后按对数时间绑定动态命名实参。
  std::vector<std::uint32_t> parameter_name_order;
};

}  // namespace vela
