#include "volt/io/text/Utf.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[utf-test] FAIL: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;

  {
    std::string encoded;
    ok = expect(volt::io::utf::appendCodepointUtf8(U'A', encoded), "encode ASCII") && ok;
    ok = expect(volt::io::utf::appendCodepointUtf8(U'\u20AC', encoded), "encode Euro") && ok;
    ok = expect(volt::io::utf::appendCodepointUtf8(U'\U0001F600', encoded), "encode emoji") && ok;

    const std::vector<char32_t> codepoints = volt::io::utf::decodeUtf8ToCodepoints(encoded);
    ok = expect(codepoints.size() == 3U, "roundtrip codepoint count") && ok;
    ok = expect(codepoints.size() >= 3U && codepoints[0] == U'A', "roundtrip ASCII") && ok;
    ok = expect(codepoints.size() >= 3U && codepoints[1] == U'\u20AC', "roundtrip Euro") && ok;
    ok = expect(codepoints.size() >= 3U && codepoints[2] == U'\U0001F600', "roundtrip emoji") && ok;
  }

  {
    const std::string invalid = std::string("a") + static_cast<char>(0xC0) + static_cast<char>(0xAF) + "z";
    const std::vector<char32_t> codepoints = volt::io::utf::decodeUtf8ToCodepoints(invalid);
    ok = expect(codepoints.size() == 4U, "invalid utf8 replacement count") && ok;
    ok = expect(codepoints.size() >= 4U && codepoints[1] == volt::io::utf::kReplacementCodepoint,
                "first invalid byte becomes replacement") && ok;
    ok = expect(codepoints.size() >= 4U && codepoints[2] == volt::io::utf::kReplacementCodepoint,
                "second invalid byte becomes replacement") && ok;
  }

  {
    char32_t cp = U'\0';
    ok = expect(volt::io::utf::decodeUtf16SurrogatePair(
                    static_cast<char16_t>(0xD83DU),
                    static_cast<char16_t>(0xDE00U),
                    cp),
                "valid surrogate pair decode") && ok;
    ok = expect(cp == U'\U0001F600', "surrogate pair codepoint value") && ok;
    ok = expect(!volt::io::utf::decodeUtf16SurrogatePair(
                    static_cast<char16_t>(0x0041U),
                    static_cast<char16_t>(0xDE00U),
                    cp),
                "invalid high surrogate rejected") && ok;
  }

  {
    ok = expect(volt::io::utf::hexToNibble('A') == 10, "hex upper") && ok;
    ok = expect(volt::io::utf::hexToNibble('f') == 15, "hex lower") && ok;
    ok = expect(volt::io::utf::hexToNibble('9') == 9, "hex digit") && ok;
    ok = expect(volt::io::utf::hexToNibble('x') < 0, "hex invalid") && ok;
  }

  if (!ok) {
    std::cerr << "[utf-test] One or more tests failed." << '\n';
    return 1;
  }

  std::cout << "[utf-test] All UTF tests passed." << '\n';
  return 0;
}