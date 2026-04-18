#include "volt/io/assets/FontAssetService.hpp"

namespace volt::io {

bool FontAssetService::ensureDefaultFontAtlas() {
  return ::volt::io::ensureDefaultFontAtlas();
}

bool FontAssetService::getDefaultFontMetrics(FontMetrics& outMetrics) {
  return ::volt::io::defaultFontMetrics(outMetrics);
}

int FontAssetService::getDefaultFontGlyphIndexForCodepoint(char32_t codepoint) {
  return ::volt::io::defaultFontGlyphIndexForCodepoint(codepoint);
}

bool FontAssetService::getDefaultFontPackedQuad(int glyphIndex,
                                                float& x,
                                                float& y,
                                                FontGlyphQuad& outQuad) {
  return ::volt::io::defaultFontPackedQuad(glyphIndex, x, y, outQuad);
}

float FontAssetService::getDefaultFontKerningAdvance(int leftGlyphIndex, int rightGlyphIndex) {
  return ::volt::io::defaultFontKerningAdvance(leftGlyphIndex, rightGlyphIndex);
}

bool FontAssetService::getDefaultFontVectorGlyph(int glyphIndex, FontVectorGlyph& outGlyph) {
  return ::volt::io::defaultFontVectorGlyph(glyphIndex, outGlyph);
}

const std::vector<FontVectorCurve>& FontAssetService::getDefaultFontVectorCurves() {
  return ::volt::io::defaultFontVectorCurves();
}

const std::string& FontAssetService::getDefaultFontTextureKey() {
  return ::volt::io::defaultFontTextureKey();
}

}  // namespace volt::io