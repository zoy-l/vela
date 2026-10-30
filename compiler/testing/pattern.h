#pragma once

#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include <pcre2.h>

namespace vela::testing {

class Pattern final {
 public:
  [[nodiscard]] static std::expected<Pattern, std::string> Regex(std::string_view source);
  [[nodiscard]] static std::expected<Pattern, std::string> Glob(std::string_view source);
  [[nodiscard]] std::expected<bool, std::string> IsMatch(std::string_view text) const;

 private:
  struct Deleter final {
    void operator()(pcre2_code* code) const noexcept;
  };
  explicit Pattern(pcre2_code* code) : code_(code) {}
  std::unique_ptr<pcre2_code, Deleter> code_;
};

}  // namespace vela::testing
