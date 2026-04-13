#pragma once

#include "volt/io/assets/Font.hpp"

namespace volt::io {

class FontAssetService {
 public:
  [[nodiscard]] bool ensureDefaultFontAtlas();
  [[nodiscard]] bool getDefaultFontMetrics(FontMetrics& outMetrics);
  [[nodiscard]] int getDefaultFontGlyphIndexForCodepoint(char32_t codepoint);
  [[nodiscard]] bool getDefaultFontPackedQuad(int glyphIndex, float& x, float& y, FontGlyphQuad& outQuad);
  [[nodiscard]] const std::string& getDefaultFontTextureKey();
};

}  // namespace volt::io