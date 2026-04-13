#include "volt/io/assets/Font.hpp"

#include "volt/io/image/ImageEncoder.hpp"
#include "volt/io/text/Utf.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace volt::io {
namespace {

struct PackedGlyph {
  float xOffset{0.0F};
  float yOffset{0.0F};
  float width{0.0F};
  float height{0.0F};
  float s0{0.0F};
  float t0{0.0F};
  float s1{0.0F};
  float t1{0.0F};
  float xAdvance{0.0F};
};

struct FontAtlasData {
  bool initialized{false};
  bool valid{false};
  float bakePixelHeight{16.0F};
  std::uint32_t atlasWidth{0};
  std::uint32_t atlasHeight{0};
  std::vector<PackedGlyph> glyphs{};
  std::unordered_map<char32_t, int> codepointToGlyphIndex{};
  char32_t fallbackCodepoint{U'?'};
  float ascentPx{14.0F};
  float descentPx{-4.0F};
  float lineGapPx{2.0F};
  std::string textureKey{"image:ui-font-default"};
};

FontAtlasData& fontAtlas() {
  static FontAtlasData atlas;
  return atlas;
}

using GlyphPattern = std::array<const char*, 7>;

const GlyphPattern& patternForAscii(char ch) {
  static const GlyphPattern kUnknown = {
      "01110",
      "10001",
      "00010",
      "00100",
      "00100",
      "00000",
      "00100",
  };

  static const GlyphPattern kSpace = {
      "00000",
      "00000",
      "00000",
      "00000",
      "00000",
      "00000",
      "00000",
  };

  if (ch >= 'a' && ch <= 'z') {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }

  switch (ch) {
    case ' ':
      return kSpace;
    case 'A': {
      static const GlyphPattern p = {"01110", "10001", "10001", "11111", "10001", "10001", "10001"};
      return p;
    }
    case 'B': {
      static const GlyphPattern p = {"11110", "10001", "10001", "11110", "10001", "10001", "11110"};
      return p;
    }
    case 'C': {
      static const GlyphPattern p = {"01110", "10001", "10000", "10000", "10000", "10001", "01110"};
      return p;
    }
    case 'D': {
      static const GlyphPattern p = {"11110", "10001", "10001", "10001", "10001", "10001", "11110"};
      return p;
    }
    case 'E': {
      static const GlyphPattern p = {"11111", "10000", "10000", "11110", "10000", "10000", "11111"};
      return p;
    }
    case 'F': {
      static const GlyphPattern p = {"11111", "10000", "10000", "11110", "10000", "10000", "10000"};
      return p;
    }
    case 'G': {
      static const GlyphPattern p = {"01110", "10001", "10000", "10111", "10001", "10001", "01110"};
      return p;
    }
    case 'H': {
      static const GlyphPattern p = {"10001", "10001", "10001", "11111", "10001", "10001", "10001"};
      return p;
    }
    case 'I': {
      static const GlyphPattern p = {"11111", "00100", "00100", "00100", "00100", "00100", "11111"};
      return p;
    }
    case 'J': {
      static const GlyphPattern p = {"00111", "00010", "00010", "00010", "10010", "10010", "01100"};
      return p;
    }
    case 'K': {
      static const GlyphPattern p = {"10001", "10010", "10100", "11000", "10100", "10010", "10001"};
      return p;
    }
    case 'L': {
      static const GlyphPattern p = {"10000", "10000", "10000", "10000", "10000", "10000", "11111"};
      return p;
    }
    case 'M': {
      static const GlyphPattern p = {"10001", "11011", "10101", "10101", "10001", "10001", "10001"};
      return p;
    }
    case 'N': {
      static const GlyphPattern p = {"10001", "11001", "10101", "10011", "10001", "10001", "10001"};
      return p;
    }
    case 'O': {
      static const GlyphPattern p = {"01110", "10001", "10001", "10001", "10001", "10001", "01110"};
      return p;
    }
    case 'P': {
      static const GlyphPattern p = {"11110", "10001", "10001", "11110", "10000", "10000", "10000"};
      return p;
    }
    case 'Q': {
      static const GlyphPattern p = {"01110", "10001", "10001", "10001", "10101", "10010", "01101"};
      return p;
    }
    case 'R': {
      static const GlyphPattern p = {"11110", "10001", "10001", "11110", "10100", "10010", "10001"};
      return p;
    }
    case 'S': {
      static const GlyphPattern p = {"01111", "10000", "10000", "01110", "00001", "00001", "11110"};
      return p;
    }
    case 'T': {
      static const GlyphPattern p = {"11111", "00100", "00100", "00100", "00100", "00100", "00100"};
      return p;
    }
    case 'U': {
      static const GlyphPattern p = {"10001", "10001", "10001", "10001", "10001", "10001", "01110"};
      return p;
    }
    case 'V': {
      static const GlyphPattern p = {"10001", "10001", "10001", "10001", "10001", "01010", "00100"};
      return p;
    }
    case 'W': {
      static const GlyphPattern p = {"10001", "10001", "10001", "10101", "10101", "11011", "10001"};
      return p;
    }
    case 'X': {
      static const GlyphPattern p = {"10001", "10001", "01010", "00100", "01010", "10001", "10001"};
      return p;
    }
    case 'Y': {
      static const GlyphPattern p = {"10001", "10001", "01010", "00100", "00100", "00100", "00100"};
      return p;
    }
    case 'Z': {
      static const GlyphPattern p = {"11111", "00001", "00010", "00100", "01000", "10000", "11111"};
      return p;
    }
    case '0': {
      static const GlyphPattern p = {"01110", "10001", "10011", "10101", "11001", "10001", "01110"};
      return p;
    }
    case '1': {
      static const GlyphPattern p = {"00100", "01100", "00100", "00100", "00100", "00100", "01110"};
      return p;
    }
    case '2': {
      static const GlyphPattern p = {"01110", "10001", "00001", "00010", "00100", "01000", "11111"};
      return p;
    }
    case '3': {
      static const GlyphPattern p = {"11110", "00001", "00001", "01110", "00001", "00001", "11110"};
      return p;
    }
    case '4': {
      static const GlyphPattern p = {"00010", "00110", "01010", "10010", "11111", "00010", "00010"};
      return p;
    }
    case '5': {
      static const GlyphPattern p = {"11111", "10000", "10000", "11110", "00001", "00001", "11110"};
      return p;
    }
    case '6': {
      static const GlyphPattern p = {"01110", "10000", "10000", "11110", "10001", "10001", "01110"};
      return p;
    }
    case '7': {
      static const GlyphPattern p = {"11111", "00001", "00010", "00100", "01000", "01000", "01000"};
      return p;
    }
    case '8': {
      static const GlyphPattern p = {"01110", "10001", "10001", "01110", "10001", "10001", "01110"};
      return p;
    }
    case '9': {
      static const GlyphPattern p = {"01110", "10001", "10001", "01111", "00001", "00001", "01110"};
      return p;
    }
    case '.': {
      static const GlyphPattern p = {"00000", "00000", "00000", "00000", "00000", "00110", "00110"};
      return p;
    }
    case ',': {
      static const GlyphPattern p = {"00000", "00000", "00000", "00000", "00110", "00110", "00100"};
      return p;
    }
    case ':': {
      static const GlyphPattern p = {"00000", "00110", "00110", "00000", "00110", "00110", "00000"};
      return p;
    }
    case ';': {
      static const GlyphPattern p = {"00000", "00110", "00110", "00000", "00110", "00110", "00100"};
      return p;
    }
    case '!': {
      static const GlyphPattern p = {"00100", "00100", "00100", "00100", "00100", "00000", "00100"};
      return p;
    }
    case '?':
      return kUnknown;
    case '-': {
      static const GlyphPattern p = {"00000", "00000", "00000", "11111", "00000", "00000", "00000"};
      return p;
    }
    case '+': {
      static const GlyphPattern p = {"00000", "00100", "00100", "11111", "00100", "00100", "00000"};
      return p;
    }
    case '/': {
      static const GlyphPattern p = {"00001", "00010", "00100", "01000", "10000", "00000", "00000"};
      return p;
    }
    case '\\': {
      static const GlyphPattern p = {"10000", "01000", "00100", "00010", "00001", "00000", "00000"};
      return p;
    }
    case '(': {
      static const GlyphPattern p = {"00010", "00100", "01000", "01000", "01000", "00100", "00010"};
      return p;
    }
    case ')': {
      static const GlyphPattern p = {"01000", "00100", "00010", "00010", "00010", "00100", "01000"};
      return p;
    }
    case '[': {
      static const GlyphPattern p = {"01110", "01000", "01000", "01000", "01000", "01000", "01110"};
      return p;
    }
    case ']': {
      static const GlyphPattern p = {"01110", "00010", "00010", "00010", "00010", "00010", "01110"};
      return p;
    }
    case '{': {
      static const GlyphPattern p = {"00010", "00100", "00100", "01000", "00100", "00100", "00010"};
      return p;
    }
    case '}': {
      static const GlyphPattern p = {"01000", "00100", "00100", "00010", "00100", "00100", "01000"};
      return p;
    }
    case '_': {
      static const GlyphPattern p = {"00000", "00000", "00000", "00000", "00000", "00000", "11111"};
      return p;
    }
    case '"': {
      static const GlyphPattern p = {"01010", "01010", "00100", "00000", "00000", "00000", "00000"};
      return p;
    }
    case '\'': {
      static const GlyphPattern p = {"00100", "00100", "00010", "00000", "00000", "00000", "00000"};
      return p;
    }
    default:
      return kUnknown;
  }
}

char normalizeAsciiCodepoint(char32_t codepoint) {
  if (!utf::isValidUnicodeScalar(codepoint)) {
    return '?';
  }

  if (codepoint < U'\u0080') {
    return static_cast<char>(codepoint);
  }

  switch (codepoint) {
    case 0x00A0:
      return ' ';
    case 0x00A1:
      return '!';
    case 0x00A9:
      return 'C';
    case 0x00AB:
    case 0x00BB:
      return '"';
    case 0x00B0:
      return 'o';
    case 0x00B7:
      return '.';
    case 0x00BF:
      return '?';
    case 0x00C0:
    case 0x00C1:
    case 0x00C2:
    case 0x00C3:
    case 0x00C4:
    case 0x00C5:
    case 0x00E0:
    case 0x00E1:
    case 0x00E2:
    case 0x00E3:
    case 0x00E4:
    case 0x00E5:
      return 'A';
    case 0x00C6:
    case 0x00E6:
      return 'A';
    case 0x00C7:
    case 0x00E7:
      return 'C';
    case 0x00C8:
    case 0x00C9:
    case 0x00CA:
    case 0x00CB:
    case 0x00E8:
    case 0x00E9:
    case 0x00EA:
    case 0x00EB:
      return 'E';
    case 0x00CC:
    case 0x00CD:
    case 0x00CE:
    case 0x00CF:
    case 0x00EC:
    case 0x00ED:
    case 0x00EE:
    case 0x00EF:
      return 'I';
    case 0x00D0:
    case 0x00F0:
      return 'D';
    case 0x00D1:
    case 0x00F1:
      return 'N';
    case 0x00D2:
    case 0x00D3:
    case 0x00D4:
    case 0x00D5:
    case 0x00D6:
    case 0x00D8:
    case 0x00F2:
    case 0x00F3:
    case 0x00F4:
    case 0x00F5:
    case 0x00F6:
    case 0x00F8:
      return 'O';
    case 0x00D9:
    case 0x00DA:
    case 0x00DB:
    case 0x00DC:
    case 0x00F9:
    case 0x00FA:
    case 0x00FB:
    case 0x00FC:
      return 'U';
    case 0x00DD:
    case 0x00FD:
    case 0x00FF:
      return 'Y';
    case 0x00DE:
    case 0x00FE:
      return 'P';
    case 0x00DF:
      return 'S';
    case 0x00D7:
      return 'x';
    case 0x00F7:
      return '/';
    case 0x2013:
    case 0x2014:
      return '-';
    case 0x2018:
    case 0x2019:
      return '\'';
    case 0x201C:
    case 0x201D:
      return '"';
    case 0x2022:
      return '*';
    case 0x2026:
      return '.';
    case 0x20AC:
      return 'E';
    default:
      break;
  }

  return '?';
}

void drawPattern(std::vector<unsigned char>& alpha,
                 std::uint32_t atlasWidth,
                 std::uint32_t atlasHeight,
                 std::uint32_t originX,
                 std::uint32_t originY,
                 const GlyphPattern& pattern,
                 int scale) {
  for (std::size_t row = 0; row < pattern.size(); ++row) {
    for (std::size_t col = 0; col < 5U; ++col) {
      if (pattern[row][col] != '1') {
        continue;
      }

      for (int sy = 0; sy < scale; ++sy) {
        for (int sx = 0; sx < scale; ++sx) {
          const std::uint32_t x = originX + static_cast<std::uint32_t>(col * static_cast<std::size_t>(scale) + static_cast<std::size_t>(sx));
          const std::uint32_t y = originY + static_cast<std::uint32_t>(row * static_cast<std::size_t>(scale) + static_cast<std::size_t>(sy));
          if (x >= atlasWidth || y >= atlasHeight) {
            continue;
          }

          alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(atlasWidth) + x] = 255U;
        }
      }
    }
  }
}

std::vector<int> buildAtlasCodepointList() {
  std::vector<int> out;
  out.reserve(256);

  for (int cp = 32; cp <= 126; ++cp) {
    out.push_back(cp);
  }
  for (int cp = 160; cp <= 255; ++cp) {
    out.push_back(cp);
  }

  const std::array<int, 18> extras = {
      0x20AC,
      0x2013,
      0x2014,
      0x2018,
      0x2019,
      0x201C,
      0x201D,
      0x2022,
      0x2026,
      0x2122,
      0x00A0,
      0x00B0,
      0x00B7,
      0x00D7,
      0x00F7,
      0x201A,
      0x2030,
      static_cast<int>('?'),
  };
  out.insert(out.end(), extras.begin(), extras.end());

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

int atlasGlyphIndexForCodepoint(const FontAtlasData& atlas, char32_t codepoint) {
  const auto it = atlas.codepointToGlyphIndex.find(codepoint);
  if (it != atlas.codepointToGlyphIndex.end()) {
    return it->second;
  }

  const auto fallbackIt = atlas.codepointToGlyphIndex.find(atlas.fallbackCodepoint);
  if (fallbackIt != atlas.codepointToGlyphIndex.end()) {
    return fallbackIt->second;
  }

  return -1;
}

}  // namespace

bool ensureDefaultFontAtlas() {
  FontAtlasData& atlas = fontAtlas();
  if (atlas.initialized) {
    return atlas.valid;
  }
  atlas.initialized = true;

  const std::vector<int> codepoints = buildAtlasCodepointList();
  atlas.glyphs.assign(codepoints.size(), PackedGlyph{});
  atlas.codepointToGlyphIndex.clear();

  constexpr std::uint32_t kCellWidth = 16U;
  constexpr std::uint32_t kCellHeight = 20U;
  constexpr std::uint32_t kGlyphDrawWidth = 10U;
  constexpr std::uint32_t kGlyphDrawHeight = 14U;
  constexpr float kAdvancePx = 11.0F;
  constexpr std::uint32_t kCols = 32U;

  const std::uint32_t rows =
      static_cast<std::uint32_t>((codepoints.size() + static_cast<std::size_t>(kCols) - 1U) / static_cast<std::size_t>(kCols));

  atlas.atlasWidth = kCellWidth * kCols;
  atlas.atlasHeight = std::max<std::uint32_t>(kCellHeight * rows, 1U);

  std::vector<unsigned char> alpha(
      static_cast<std::size_t>(atlas.atlasWidth) * static_cast<std::size_t>(atlas.atlasHeight),
      0U);

  for (std::size_t i = 0; i < codepoints.size(); ++i) {
    const std::uint32_t col = static_cast<std::uint32_t>(i % kCols);
    const std::uint32_t row = static_cast<std::uint32_t>(i / kCols);

    const std::uint32_t cellX = col * kCellWidth;
    const std::uint32_t cellY = row * kCellHeight;

    const std::uint32_t drawX = cellX + ((kCellWidth - kGlyphDrawWidth) / 2U);
    const std::uint32_t drawY = cellY + 2U;

    const char32_t cp = static_cast<char32_t>(codepoints[i]);
    const char glyphChar = normalizeAsciiCodepoint(cp);
    const GlyphPattern& pattern = patternForAscii(glyphChar);
    drawPattern(alpha, atlas.atlasWidth, atlas.atlasHeight, drawX, drawY, pattern, 2);

    PackedGlyph glyph{};
    glyph.xOffset = 1.0F;
    glyph.yOffset = -12.0F;
    glyph.width = glyphChar == ' ' ? 0.0F : static_cast<float>(kGlyphDrawWidth);
    glyph.height = glyphChar == ' ' ? 0.0F : static_cast<float>(kGlyphDrawHeight);
    glyph.s0 = static_cast<float>(drawX) / static_cast<float>(atlas.atlasWidth);
    glyph.t0 = static_cast<float>(drawY) / static_cast<float>(atlas.atlasHeight);
    glyph.s1 = static_cast<float>(drawX + kGlyphDrawWidth) / static_cast<float>(atlas.atlasWidth);
    glyph.t1 = static_cast<float>(drawY + kGlyphDrawHeight) / static_cast<float>(atlas.atlasHeight);
    glyph.xAdvance = glyphChar == ' ' ? 6.0F : kAdvancePx;

    atlas.glyphs[i] = glyph;
    atlas.codepointToGlyphIndex.emplace(cp, static_cast<int>(i));
  }

  RawImage atlasImage{};
  atlasImage.width = atlas.atlasWidth;
  atlasImage.height = atlas.atlasHeight;
  atlasImage.rgba.resize(static_cast<std::size_t>(atlas.atlasWidth) * static_cast<std::size_t>(atlas.atlasHeight) * 4U);

  for (std::size_t i = 0; i < alpha.size(); ++i) {
    const std::size_t px = i * 4U;
    atlasImage.rgba[px + 0U] = 255U;
    atlasImage.rgba[px + 1U] = 255U;
    atlasImage.rgba[px + 2U] = 255U;
    atlasImage.rgba[px + 3U] = alpha[i];
  }

  const std::filesystem::path atlasPath = std::filesystem::path("assets/images/ui-font-default.png");
  std::error_code ec;
  if (std::filesystem::exists(atlasPath, ec) && !ec) {
    atlas.valid = true;
    return true;
  }

  atlas.valid = encodeImageFile(atlasPath, atlasImage, ImageEncodeFormat::kPng);
  return atlas.valid;
}

bool defaultFontMetrics(FontMetrics& outMetrics) {
  if (!ensureDefaultFontAtlas()) {
    return false;
  }

  const FontAtlasData& atlas = fontAtlas();
  outMetrics.bakePixelHeight = atlas.bakePixelHeight;
  outMetrics.ascentPx = atlas.ascentPx;
  outMetrics.descentPx = atlas.descentPx;
  outMetrics.lineGapPx = atlas.lineGapPx;
  return true;
}

int defaultFontGlyphIndexForCodepoint(char32_t codepoint) {
  if (!ensureDefaultFontAtlas()) {
    return -1;
  }

  if (!utf::isValidUnicodeScalar(codepoint)) {
    codepoint = U'?';
  }

  return atlasGlyphIndexForCodepoint(fontAtlas(), codepoint);
}

bool defaultFontPackedQuad(int glyphIndex, float& x, float& y, FontGlyphQuad& outQuad) {
  if (!ensureDefaultFontAtlas()) {
    return false;
  }

  const FontAtlasData& atlas = fontAtlas();
  if (glyphIndex < 0 || glyphIndex >= static_cast<int>(atlas.glyphs.size())) {
    return false;
  }

  const PackedGlyph& glyph = atlas.glyphs[static_cast<std::size_t>(glyphIndex)];

  outQuad.x0 = x + glyph.xOffset;
  outQuad.y0 = y + glyph.yOffset;
  outQuad.x1 = outQuad.x0 + glyph.width;
  outQuad.y1 = outQuad.y0 + glyph.height;
  outQuad.s0 = glyph.s0;
  outQuad.t0 = glyph.t0;
  outQuad.s1 = glyph.s1;
  outQuad.t1 = glyph.t1;

  x += glyph.xAdvance;
  return true;
}

const std::string& defaultFontTextureKey() {
  return fontAtlas().textureKey;
}

}  // namespace volt::io
