#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>
#include <utility>

namespace volt::core {

class Text {
 public:
  Text() = default;
  explicit Text(std::u32string codepoints);
  explicit Text(std::string_view utf8);

  [[nodiscard]] static Text fromUtf8(std::string_view utf8);
  [[nodiscard]] static Text fromUtf16(std::u16string_view utf16);
  [[nodiscard]] static bool hasIcuBackend();

  [[nodiscard]] const std::u32string& codepoints() const;
  [[nodiscard]] std::u32string_view view() const;
  [[nodiscard]] bool empty() const;
  [[nodiscard]] std::size_t size() const;

  [[nodiscard]] std::string toUtf8() const;
  [[nodiscard]] std::u16string toUtf16() const;

  [[nodiscard]] Text normalizedNfc() const;
  [[nodiscard]] Text caseFolded() const;
  [[nodiscard]] int localeCompare(const Text& other, std::string_view localeName = {}) const;

  auto operator<=>(const Text&) const = default;

 private:
  std::u32string codepoints_{};
};

std::ostream& operator<<(std::ostream& out, const Text& text);

}  // namespace volt::core
