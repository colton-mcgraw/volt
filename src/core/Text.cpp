#include "volt/core/Text.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <locale>
#include <ostream>
#include <stdexcept>

#if defined(VOLT_HAS_ICU) && VOLT_HAS_ICU
#include <unicode/ucol.h>
#include <unicode/ustring.h>
#include <unicode/unorm2.h>
#include <unicode/utypes.h>
#endif

namespace {

constexpr char32_t kReplacementCodepoint = U'\uFFFD';

[[nodiscard]] bool isHighSurrogate(char16_t value) {
  return value >= static_cast<char16_t>(0xD800U) && value <= static_cast<char16_t>(0xDBFFU);
}

[[nodiscard]] bool isLowSurrogate(char16_t value) {
  return value >= static_cast<char16_t>(0xDC00U) && value <= static_cast<char16_t>(0xDFFFU);
}

[[nodiscard]] bool isValidUnicodeScalar(char32_t value) {
  if (value > U'\U0010FFFF') {
    return false;
  }

  const std::uint32_t scalar = static_cast<std::uint32_t>(value);
  return !(scalar >= 0xD800U && scalar <= 0xDFFFU);
}

[[nodiscard]] bool sizeFitsI32(std::size_t value) {
  return value <= static_cast<std::size_t>(std::numeric_limits<int32_t>::max());
}

[[nodiscard]] int compareCodepointOrder(std::u32string_view lhs, std::u32string_view rhs) {
  if (lhs < rhs) {
    return -1;
  }
  if (lhs > rhs) {
    return 1;
  }
  return 0;
}

[[nodiscard]] std::u16string encodeUtf16FromCodepoints(std::u32string_view codepoints) {
  std::u16string out;
  out.reserve(codepoints.size());

  for (const char32_t cp : codepoints) {
    const std::uint32_t scalar = isValidUnicodeScalar(cp)
                                     ? static_cast<std::uint32_t>(cp)
                                     : static_cast<std::uint32_t>(kReplacementCodepoint);
    if (scalar <= 0xFFFFU) {
      out.push_back(static_cast<char16_t>(scalar));
      continue;
    }

    const std::uint32_t adjusted = scalar - 0x10000U;
    out.push_back(static_cast<char16_t>(0xD800U + ((adjusted >> 10U) & 0x3FFU)));
    out.push_back(static_cast<char16_t>(0xDC00U + (adjusted & 0x3FFU)));
  }

  return out;
}

[[nodiscard]] std::u32string decodeUtf16ToCodepoints(std::u16string_view utf16) {
  std::u32string out;
  out.reserve(utf16.size());

  std::size_t i = 0U;
  while (i < utf16.size()) {
    const char16_t unit = utf16[i];
    if (isHighSurrogate(unit)) {
      if (i + 1U < utf16.size() && isLowSurrogate(utf16[i + 1U])) {
        const std::uint32_t high = static_cast<std::uint32_t>(unit);
        const std::uint32_t low = static_cast<std::uint32_t>(utf16[i + 1U]);
        out.push_back(
            static_cast<char32_t>(0x10000U + ((high - 0xD800U) << 10U) + (low - 0xDC00U)));
        i += 2U;
        continue;
      }
      out.push_back(kReplacementCodepoint);
      ++i;
      continue;
    }

    if (isLowSurrogate(unit)) {
      out.push_back(kReplacementCodepoint);
      ++i;
      continue;
    }

    out.push_back(static_cast<char32_t>(unit));
    ++i;
  }

  return out;
}

void appendCodepointUtf8(char32_t codepoint, std::string& outUtf8) {
  if (!isValidUnicodeScalar(codepoint)) {
    codepoint = kReplacementCodepoint;
  }

  const std::uint32_t scalar = static_cast<std::uint32_t>(codepoint);

  if (scalar <= 0x7FU) {
    outUtf8.push_back(static_cast<char>(scalar));
    return;
  }

  if (scalar <= 0x7FFU) {
    outUtf8.push_back(static_cast<char>(0xC0U | ((scalar >> 6U) & 0x1FU)));
    outUtf8.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    return;
  }

  if (scalar <= 0xFFFFU) {
    outUtf8.push_back(static_cast<char>(0xE0U | ((scalar >> 12U) & 0x0FU)));
    outUtf8.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
    outUtf8.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    return;
  }

  outUtf8.push_back(static_cast<char>(0xF0U | ((scalar >> 18U) & 0x07U)));
  outUtf8.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3FU)));
  outUtf8.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
  outUtf8.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
}

[[nodiscard]] std::u32string decodeUtf8ToCodepoints(std::string_view utf8Text) {
  std::u32string codepoints;
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

[[nodiscard]] char32_t foldAscii(char32_t cp) {
  if (cp >= U'A' && cp <= U'Z') {
    return static_cast<char32_t>(cp + (U'a' - U'A'));
  }
  return cp;
}

}  // namespace

namespace volt::core {

Text::Text(std::u32string codepoints)
    : codepoints_(std::move(codepoints)) {}

Text::Text(std::string_view utf8)
    : codepoints_(decodeUtf8ToCodepoints(utf8)) {}

Text Text::fromUtf8(std::string_view utf8) {
  return Text(utf8);
}

Text Text::fromUtf16(std::u16string_view utf16) {
  return Text(decodeUtf16ToCodepoints(utf16));
}

bool Text::hasIcuBackend() {
#if defined(VOLT_HAS_ICU) && VOLT_HAS_ICU
  return true;
#else
  return false;
#endif
}

const std::u32string& Text::codepoints() const {
  return codepoints_;
}

std::u32string_view Text::view() const {
  return std::u32string_view(codepoints_.data(), codepoints_.size());
}

bool Text::empty() const {
  return codepoints_.empty();
}

std::size_t Text::size() const {
  return codepoints_.size();
}

std::string Text::toUtf8() const {
  std::string out;
  out.reserve(codepoints_.size());
  for (const char32_t cp : codepoints_) {
    appendCodepointUtf8(cp, out);
  }
  return out;
}

std::u16string Text::toUtf16() const {
  return encodeUtf16FromCodepoints(codepoints_);
}

Text Text::normalizedNfc() const {
#if defined(VOLT_HAS_ICU) && VOLT_HAS_ICU
  static_assert(sizeof(UChar) == sizeof(char16_t), "ICU UChar must match char16_t width");

  const std::u16string source = toUtf16();
  if (!sizeFitsI32(source.size())) {
    return Text(codepoints_);
  }

  UErrorCode status = U_ZERO_ERROR;
  const UNormalizer2* normalizer = unorm2_getNFCInstance(&status);
  if (U_FAILURE(status) || normalizer == nullptr) {
    return Text(codepoints_);
  }

  const int32_t sourceLen = static_cast<int32_t>(source.size());
  status = U_ZERO_ERROR;
  const int32_t requiredLen = unorm2_normalize(
      normalizer,
      reinterpret_cast<const UChar*>(source.data()),
      sourceLen,
      nullptr,
      0,
      &status);
  if (!(status == U_BUFFER_OVERFLOW_ERROR || U_SUCCESS(status))) {
    return Text(codepoints_);
  }

  std::u16string normalized(static_cast<std::size_t>(requiredLen), u'\0');
  status = U_ZERO_ERROR;
  const int32_t writtenLen = unorm2_normalize(
      normalizer,
      reinterpret_cast<const UChar*>(source.data()),
      sourceLen,
      reinterpret_cast<UChar*>(normalized.data()),
      requiredLen,
      &status);
  if (U_FAILURE(status)) {
    return Text(codepoints_);
  }

  normalized.resize(static_cast<std::size_t>(writtenLen));
  return Text::fromUtf16(normalized);
#else
  return Text(codepoints_);
#endif
}

Text Text::caseFolded() const {
#if defined(VOLT_HAS_ICU) && VOLT_HAS_ICU
  static_assert(sizeof(UChar) == sizeof(char16_t), "ICU UChar must match char16_t width");

  const std::u16string source = toUtf16();
  if (!sizeFitsI32(source.size())) {
    return Text(codepoints_);
  }

  const int32_t sourceLen = static_cast<int32_t>(source.size());
  UErrorCode status = U_ZERO_ERROR;
  const int32_t requiredLen = u_strFoldCase(
      nullptr,
      0,
      reinterpret_cast<const UChar*>(source.data()),
      sourceLen,
      U_FOLD_CASE_DEFAULT,
      &status);
  if (!(status == U_BUFFER_OVERFLOW_ERROR || U_SUCCESS(status))) {
    return Text(codepoints_);
  }

  std::u16string folded(static_cast<std::size_t>(requiredLen), u'\0');
  status = U_ZERO_ERROR;
  const int32_t writtenLen = u_strFoldCase(
      reinterpret_cast<UChar*>(folded.data()),
      requiredLen,
      reinterpret_cast<const UChar*>(source.data()),
      sourceLen,
      U_FOLD_CASE_DEFAULT,
      &status);
  if (U_FAILURE(status)) {
    return Text(codepoints_);
  }

  folded.resize(static_cast<std::size_t>(writtenLen));
  return Text::fromUtf16(folded);
#else
  std::u32string folded = codepoints_;
  std::transform(folded.begin(), folded.end(), folded.begin(), [](char32_t cp) {
    return foldAscii(cp);
  });
  return Text(std::move(folded));
#endif
}

int Text::localeCompare(const Text& other, std::string_view localeName) const {
#if defined(VOLT_HAS_ICU) && VOLT_HAS_ICU
  static_assert(sizeof(UChar) == sizeof(char16_t), "ICU UChar must match char16_t width");

  const std::u16string lhs = toUtf16();
  const std::u16string rhs = other.toUtf16();
  if (!sizeFitsI32(lhs.size()) || !sizeFitsI32(rhs.size())) {
    return compareCodepointOrder(codepoints_, other.codepoints_);
  }

  std::string localeStorage(localeName);
  UErrorCode status = U_ZERO_ERROR;
  UCollator* collator =
      ucol_open(localeStorage.empty() ? nullptr : localeStorage.c_str(), &status);
  if (U_FAILURE(status) || collator == nullptr) {
    return compareCodepointOrder(codepoints_, other.codepoints_);
  }

  const UCollationResult result = ucol_strcoll(
      collator,
      reinterpret_cast<const UChar*>(lhs.data()),
      static_cast<int32_t>(lhs.size()),
      reinterpret_cast<const UChar*>(rhs.data()),
      static_cast<int32_t>(rhs.size()));
  ucol_close(collator);

  if (result == UCOL_LESS) {
    return -1;
  }
  if (result == UCOL_GREATER) {
    return 1;
  }
  return 0;
#else
  const std::string lhsUtf8 = toUtf8();
  const std::string rhsUtf8 = other.toUtf8();

  try {
    const std::locale locale = localeName.empty()
                                   ? std::locale("")
                                   : std::locale(std::string(localeName).c_str());
    const auto& collator = std::use_facet<std::collate<char>>(locale);
    return collator.compare(
        lhsUtf8.data(),
        lhsUtf8.data() + lhsUtf8.size(),
        rhsUtf8.data(),
        rhsUtf8.data() + rhsUtf8.size());
  } catch (const std::runtime_error&) {
    return compareCodepointOrder(codepoints_, other.codepoints_);
  }
#endif
}

std::ostream& operator<<(std::ostream& out, const Text& text) {
  out << text.toUtf8();
  return out;
}

}  // namespace volt::core
