#include "volt/io/text/Utf.hpp"

namespace volt::io::utf {

int hexToNibble(char ch) {
  if (ch >= '0' && ch <= '9') {
    return static_cast<int>(ch - '0');
  }
  if (ch >= 'a' && ch <= 'f') {
    return static_cast<int>(ch - 'a' + 10);
  }
  if (ch >= 'A' && ch <= 'F') {
    return static_cast<int>(ch - 'A' + 10);
  }
  return -1;
}

bool isHighSurrogate(char16_t value) {
  return value >= static_cast<char16_t>(0xD800U) && value <= static_cast<char16_t>(0xDBFFU);
}

bool isLowSurrogate(char16_t value) {
  return value >= static_cast<char16_t>(0xDC00U) && value <= static_cast<char16_t>(0xDFFFU);
}

bool isValidUnicodeScalar(char32_t value) {
  if (value > U'\U0010FFFF') {
    return false;
  }

  const std::uint32_t scalar = static_cast<std::uint32_t>(value);
  return !(scalar >= 0xD800U && scalar <= 0xDFFFU);
}

bool decodeUtf16SurrogatePair(
    char16_t highSurrogate,
    char16_t lowSurrogate,
    char32_t& outCodepoint) {
  if (!isHighSurrogate(highSurrogate) || !isLowSurrogate(lowSurrogate)) {
    return false;
  }

  const std::uint32_t high = static_cast<std::uint32_t>(highSurrogate);
  const std::uint32_t low = static_cast<std::uint32_t>(lowSurrogate);
  outCodepoint =
      static_cast<char32_t>(0x10000U + ((high - 0xD800U) << 10U) + (low - 0xDC00U));
  return true;
}

bool appendCodepointUtf8(char32_t codepoint, std::string& outUtf8) {
  if (!isValidUnicodeScalar(codepoint)) {
    return false;
  }

  const std::uint32_t scalar = static_cast<std::uint32_t>(codepoint);

  if (scalar <= 0x7FU) {
    outUtf8.push_back(static_cast<char>(scalar));
    return true;
  }

  if (scalar <= 0x7FFU) {
    outUtf8.push_back(static_cast<char>(0xC0U | ((scalar >> 6U) & 0x1FU)));
    outUtf8.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    return true;
  }

  if (scalar <= 0xFFFFU) {
    outUtf8.push_back(static_cast<char>(0xE0U | ((scalar >> 12U) & 0x0FU)));
    outUtf8.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
    outUtf8.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    return true;
  }

  outUtf8.push_back(static_cast<char>(0xF0U | ((scalar >> 18U) & 0x07U)));
  outUtf8.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3FU)));
  outUtf8.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
  outUtf8.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
  return true;
}

std::vector<char32_t> decodeUtf8ToCodepoints(std::string_view utf8Text) {
  std::vector<char32_t> codepoints;
  codepoints.reserve(utf8Text.size());

  std::size_t i = 0U;
  while (i < utf8Text.size()) {
    const std::uint8_t lead = static_cast<std::uint8_t>(utf8Text[i]);

    if (lead <= 0x7FU) {
      codepoints.push_back(static_cast<char32_t>(lead));
      ++i;
      continue;
    }

    auto invalid = [&]() {
      codepoints.push_back(kReplacementCodepoint);
      ++i;
    };

    if (lead >= 0xC2U && lead <= 0xDFU) {
      if (i + 1U >= utf8Text.size()) {
        invalid();
        continue;
      }

      const std::uint8_t b1 = static_cast<std::uint8_t>(utf8Text[i + 1U]);
      if ((b1 & 0xC0U) != 0x80U) {
        invalid();
        continue;
      }

      const std::uint32_t cp =
          (static_cast<std::uint32_t>(lead & 0x1FU) << 6U) |
          static_cast<std::uint32_t>(b1 & 0x3FU);
      codepoints.push_back(static_cast<char32_t>(cp));
      i += 2U;
      continue;
    }

    if (lead >= 0xE0U && lead <= 0xEFU) {
      if (i + 2U >= utf8Text.size()) {
        invalid();
        continue;
      }

      const std::uint8_t b1 = static_cast<std::uint8_t>(utf8Text[i + 1U]);
      const std::uint8_t b2 = static_cast<std::uint8_t>(utf8Text[i + 2U]);
      if ((b1 & 0xC0U) != 0x80U || (b2 & 0xC0U) != 0x80U) {
        invalid();
        continue;
      }

      if ((lead == 0xE0U && b1 < 0xA0U) || (lead == 0xEDU && b1 >= 0xA0U)) {
        invalid();
        continue;
      }

      const std::uint32_t cp =
          (static_cast<std::uint32_t>(lead & 0x0FU) << 12U) |
          (static_cast<std::uint32_t>(b1 & 0x3FU) << 6U) |
          static_cast<std::uint32_t>(b2 & 0x3FU);

      codepoints.push_back(isValidUnicodeScalar(static_cast<char32_t>(cp))
                               ? static_cast<char32_t>(cp)
                               : kReplacementCodepoint);
      i += 3U;
      continue;
    }

    if (lead >= 0xF0U && lead <= 0xF4U) {
      if (i + 3U >= utf8Text.size()) {
        invalid();
        continue;
      }

      const std::uint8_t b1 = static_cast<std::uint8_t>(utf8Text[i + 1U]);
      const std::uint8_t b2 = static_cast<std::uint8_t>(utf8Text[i + 2U]);
      const std::uint8_t b3 = static_cast<std::uint8_t>(utf8Text[i + 3U]);
      if ((b1 & 0xC0U) != 0x80U || (b2 & 0xC0U) != 0x80U || (b3 & 0xC0U) != 0x80U) {
        invalid();
        continue;
      }

      if ((lead == 0xF0U && b1 < 0x90U) || (lead == 0xF4U && b1 > 0x8FU)) {
        invalid();
        continue;
      }

      const std::uint32_t cp =
          (static_cast<std::uint32_t>(lead & 0x07U) << 18U) |
          (static_cast<std::uint32_t>(b1 & 0x3FU) << 12U) |
          (static_cast<std::uint32_t>(b2 & 0x3FU) << 6U) |
          static_cast<std::uint32_t>(b3 & 0x3FU);

      codepoints.push_back(isValidUnicodeScalar(static_cast<char32_t>(cp))
                               ? static_cast<char32_t>(cp)
                               : kReplacementCodepoint);
      i += 4U;
      continue;
    }

    invalid();
  }

  return codepoints;
}

}  // namespace volt::io::utf