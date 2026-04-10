#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace volt::ui {

struct GlyphCluster {
  std::uint32_t codepoint{0};
  std::uint32_t byteOffset{0};
};

struct TextRun {
  std::string utf8Text;
  std::string fontFamily{"default"};
  float fontSizePx{14.0F};
  std::vector<GlyphCluster> glyphs;
};

[[nodiscard]] bool decodeUtf8(std::string_view text, std::vector<GlyphCluster>* outGlyphs);
[[nodiscard]] TextRun buildTextRun(std::string_view text, std::string fontFamily, float fontSizePx);

}  // namespace volt::ui
