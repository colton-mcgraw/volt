#pragma once

#include <cstdint>
#include <string>

namespace volt::io {

struct FontMetrics {
  float bakePixelHeight{32.0F};
  float ascentPx{0.0F};
  float descentPx{0.0F};
  float lineGapPx{0.0F};
};

struct FontGlyphQuad {
  float x0{0.0F};
  float y0{0.0F};
  float x1{0.0F};
  float y1{0.0F};
  float s0{0.0F};
  float t0{0.0F};
  float s1{0.0F};
  float t1{0.0F};
};

[[nodiscard]] bool ensureDefaultFontAtlas();
[[nodiscard]] bool defaultFontMetrics(FontMetrics& outMetrics);
[[nodiscard]] int defaultFontGlyphIndexForCodepoint(char32_t codepoint);
[[nodiscard]] bool defaultFontPackedQuad(int glyphIndex, float& x, float& y, FontGlyphQuad& outQuad);
[[nodiscard]] const std::string& defaultFontTextureKey();

}  // namespace volt::io
