#include "volt/core/Text.hpp"

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[text-test] FAIL: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;

  {
    const volt::core::Text text = volt::core::Text::fromUtf8("A\xE2\x82\xAC\xF0\x9F\x98\x80");
    ok = expect(text.size() == 3U, "utf8 decode count") && ok;
    ok = expect(text.toUtf8() == "A\xE2\x82\xAC\xF0\x9F\x98\x80", "utf8 roundtrip") && ok;
  }

  {
    const std::string invalid = std::string("a") + static_cast<char>(0xC0) + static_cast<char>(0xAF) + "z";
    const volt::core::Text text = volt::core::Text::fromUtf8(invalid);
    ok = expect(text.size() == 4U, "invalid utf8 replacement count") && ok;
    ok = expect(text.toUtf8() == "a\xEF\xBF\xBD\xEF\xBF\xBDz", "invalid utf8 replacement value") && ok;
  }

  {
    const std::u16string utf16 = {
        static_cast<char16_t>(0x0041),
        static_cast<char16_t>(0xD83D),
        static_cast<char16_t>(0xDE00),
    };
    const volt::core::Text text = volt::core::Text::fromUtf16(utf16);
    ok = expect(text.toUtf8() == "A\xF0\x9F\x98\x80", "utf16 to utf8 conversion") && ok;
  }

  {
    const volt::core::Text lhs = volt::core::Text::fromUtf8("Alpha");
    const volt::core::Text rhs = volt::core::Text::fromUtf8("alpha");
    ok = expect(lhs.caseFolded() == rhs.caseFolded(), "ascii case fold") && ok;
    ok = expect(lhs.localeCompare(rhs) != 0, "locale compare distinguishes case") && ok;
  }

  {
    const bool hasIcu = volt::core::Text::hasIcuBackend();
    const volt::core::Text decomposed = volt::core::Text::fromUtf8("e\xCC\x81");
    const volt::core::Text composed = volt::core::Text::fromUtf8("\xC3\xA9");
    if (hasIcu) {
      ok = expect(
               decomposed.normalizedNfc() == composed.normalizedNfc(),
               "ICU NFC normalization canonical equivalence") && ok;
    } else {
      ok = expect(
               decomposed.normalizedNfc() == decomposed,
               "fallback NFC normalization is identity") && ok;
    }
  }

  {
    const bool hasIcu = volt::core::Text::hasIcuBackend();
    const volt::core::Text sharpS = volt::core::Text::fromUtf8("Stra\xC3\x9F" "e");
    const volt::core::Text ss = volt::core::Text::fromUtf8("STRASSE");
    if (hasIcu) {
      ok = expect(
               sharpS.caseFolded() == ss.caseFolded(),
               "ICU case fold handles sharp-s equivalence") && ok;
    } else {
      ok = expect(
               sharpS.caseFolded() != ss.caseFolded(),
               "fallback case fold remains ASCII-only") && ok;
    }
  }

  {
    const volt::core::Text lhs = volt::core::Text::fromUtf8("apple");
    const volt::core::Text rhs = volt::core::Text::fromUtf8("banana");
    const int forward = lhs.localeCompare(rhs, "en_US");
    const int reverse = rhs.localeCompare(lhs, "en_US");
    ok = expect((forward < 0 && reverse > 0) || (forward > 0 && reverse < 0),
                "locale compare antisymmetry") && ok;
  }

  {
    const volt::core::Text sameA = volt::core::Text::fromUtf8("resume");
    const volt::core::Text sameB = volt::core::Text::fromUtf8("resume");
    ok = expect(sameA.localeCompare(sameB, "en_US") == 0, "locale compare equality") && ok;
  }

  if (!ok) {
    std::cerr << "[text-test] One or more tests failed." << '\n';
    return 1;
  }

  std::cout << "[text-test] All Text tests passed." << '\n';
  return 0;
}
