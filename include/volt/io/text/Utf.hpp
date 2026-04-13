#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace volt::io::utf {

constexpr char32_t kReplacementCodepoint = U'\uFFFD';

[[nodiscard]] int hexToNibble(char ch);
[[nodiscard]] bool isHighSurrogate(char16_t value);
[[nodiscard]] bool isLowSurrogate(char16_t value);
[[nodiscard]] bool isValidUnicodeScalar(char32_t value);

[[nodiscard]] bool decodeUtf16SurrogatePair(
    char16_t highSurrogate,
    char16_t lowSurrogate,
    char32_t& outCodepoint);

[[nodiscard]] bool appendCodepointUtf8(
    char32_t codepoint,
    std::string& outUtf8);

[[nodiscard]] std::vector<char32_t> decodeUtf8ToCodepoints(
    std::string_view utf8Text);

}  // namespace volt::io::utf