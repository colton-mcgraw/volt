#include "volt/ui/UIMesh.hpp"

#include "volt/io/ImageCodec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

namespace volt::ui {
namespace {

void appendQuad(
    const Rect& bounds,
    const Color& color,
    std::uint64_t widgetId,
  const std::string& textureKey,
    UiMeshData* mesh) {
  if (mesh == nullptr || bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return;
  }

  const std::uint32_t baseVertex = static_cast<std::uint32_t>(mesh->vertices.size());
  const std::uint32_t firstIndex = static_cast<std::uint32_t>(mesh->indices.size());

  mesh->vertices.push_back(UiVertex{bounds.x, bounds.y, 0.0F, 0.0F, color});
  mesh->vertices.push_back(UiVertex{bounds.x + bounds.width, bounds.y, 1.0F, 0.0F, color});
  mesh->vertices.push_back(UiVertex{bounds.x + bounds.width, bounds.y + bounds.height, 1.0F, 1.0F, color});
  mesh->vertices.push_back(UiVertex{bounds.x, bounds.y + bounds.height, 0.0F, 1.0F, color});

  mesh->indices.push_back(baseVertex + 0U);
  mesh->indices.push_back(baseVertex + 1U);
  mesh->indices.push_back(baseVertex + 2U);
  mesh->indices.push_back(baseVertex + 0U);
  mesh->indices.push_back(baseVertex + 2U);
  mesh->indices.push_back(baseVertex + 3U);

  mesh->batches.push_back(UiMeshBatch{
      widgetId,
      firstIndex,
      6U,
      bounds,
      textureKey,
  });
}

void appendQuadGeometryUv(
    const Rect& bounds,
    const Color& color,
    float u0,
    float v0,
    float u1,
    float v1,
    UiMeshData* mesh) {
  if (mesh == nullptr || bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return;
  }

  const std::uint32_t baseVertex = static_cast<std::uint32_t>(mesh->vertices.size());

  mesh->vertices.push_back(UiVertex{bounds.x, bounds.y, u0, v0, color});
  mesh->vertices.push_back(UiVertex{bounds.x + bounds.width, bounds.y, u1, v0, color});
  mesh->vertices.push_back(UiVertex{bounds.x + bounds.width, bounds.y + bounds.height, u1, v1, color});
  mesh->vertices.push_back(UiVertex{bounds.x, bounds.y + bounds.height, u0, v1, color});

  mesh->indices.push_back(baseVertex + 0U);
  mesh->indices.push_back(baseVertex + 1U);
  mesh->indices.push_back(baseVertex + 2U);
  mesh->indices.push_back(baseVertex + 0U);
  mesh->indices.push_back(baseVertex + 2U);
  mesh->indices.push_back(baseVertex + 3U);
}

struct FontAtlasData {
  bool initialized{false};
  bool valid{false};
  float bakePixelHeight{32.0F};
  std::uint32_t atlasWidth{1024};
  std::uint32_t atlasHeight{1024};
  std::vector<stbtt_packedchar> glyphs{};
  std::unordered_map<std::uint32_t, int> codepointToGlyphIndex{};
  std::uint32_t fallbackCodepoint{static_cast<std::uint32_t>('?')};
  float ascentPx{0.0F};
  float descentPx{0.0F};
  float lineGapPx{0.0F};
  std::string textureKey{"image:ui-font-default"};
};

FontAtlasData& fontAtlas() {
  static FontAtlasData atlas;
  return atlas;
}

std::vector<unsigned char> readBinary(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return {};
  }

  return std::vector<unsigned char>(
      std::istreambuf_iterator<char>(in),
      std::istreambuf_iterator<char>());
}

std::vector<std::uint32_t> decodeUtf8ToCodepoints(const std::string& text) {
  std::vector<std::uint32_t> codepoints;
  codepoints.reserve(text.size());

  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    std::uint32_t cp = 0xFFFDU;
    std::size_t advance = 1;

    if ((lead & 0x80U) == 0U) {
      cp = lead;
      advance = 1;
    } else if ((lead & 0xE0U) == 0xC0U && i + 1 < text.size()) {
      const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
      if ((b1 & 0xC0U) == 0x80U) {
        cp = (static_cast<std::uint32_t>(lead & 0x1FU) << 6U) |
             static_cast<std::uint32_t>(b1 & 0x3FU);
        advance = 2;
      }
    } else if ((lead & 0xF0U) == 0xE0U && i + 2 < text.size()) {
      const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
      if ((b1 & 0xC0U) == 0x80U && (b2 & 0xC0U) == 0x80U) {
        cp = (static_cast<std::uint32_t>(lead & 0x0FU) << 12U) |
             (static_cast<std::uint32_t>(b1 & 0x3FU) << 6U) |
             static_cast<std::uint32_t>(b2 & 0x3FU);
        advance = 3;
      }
    } else if ((lead & 0xF8U) == 0xF0U && i + 3 < text.size()) {
      const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
      const unsigned char b3 = static_cast<unsigned char>(text[i + 3]);
      if ((b1 & 0xC0U) == 0x80U && (b2 & 0xC0U) == 0x80U && (b3 & 0xC0U) == 0x80U) {
        cp = (static_cast<std::uint32_t>(lead & 0x07U) << 18U) |
             (static_cast<std::uint32_t>(b1 & 0x3FU) << 12U) |
             (static_cast<std::uint32_t>(b2 & 0x3FU) << 6U) |
             static_cast<std::uint32_t>(b3 & 0x3FU);
        advance = 4;
      }
    }

    if (cp > 0x10FFFFU) {
      cp = 0xFFFDU;
    }

    codepoints.push_back(cp);
    i += advance;
  }

  return codepoints;
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
      0x20AC, // Euro
      0x2013, // en dash
      0x2014, // em dash
      0x2018, // left single quote
      0x2019, // right single quote
      0x201C, // left double quote
      0x201D, // right double quote
      0x2022, // bullet
      0x2026, // ellipsis
      0x2122, // trademark
      0x00A0, // nbsp
      0x00B0, // degree
      0x00B7, // middle dot
      0x00D7, // multiplication
      0x00F7, // division
      0x201A, // single low-9 quote
      0x2030, // per mille
      static_cast<int>('?'),
  };
  out.insert(out.end(), extras.begin(), extras.end());

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

int atlasGlyphIndexForCodepoint(const FontAtlasData& atlas, std::uint32_t codepoint) {
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

bool ensureFontAtlas() {
  FontAtlasData& atlas = fontAtlas();
  if (atlas.initialized) {
    return atlas.valid;
  }
  atlas.initialized = true;

  const std::array<std::filesystem::path, 3> fontCandidates = {
      std::filesystem::path("assets/fonts/DefaultFont.ttf"),
      std::filesystem::path("assets/fonts/default.ttf"),
      std::filesystem::path("C:/Windows/Fonts/arial.ttf"),
  };

  std::vector<unsigned char> fontBytes;
  for (const auto& candidate : fontCandidates) {
    fontBytes = readBinary(candidate);
    if (!fontBytes.empty()) {
      break;
    }
  }

  if (fontBytes.empty()) {
    return false;
  }

  stbtt_fontinfo fontInfo{};
  if (stbtt_InitFont(&fontInfo, fontBytes.data(), 0) == 0) {
    return false;
  }

  int ascent = 0;
  int descent = 0;
  int lineGap = 0;
  stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);
  const float bakeScale = stbtt_ScaleForPixelHeight(&fontInfo, atlas.bakePixelHeight);
  atlas.ascentPx = static_cast<float>(ascent) * bakeScale;
  atlas.descentPx = static_cast<float>(descent) * bakeScale;
  atlas.lineGapPx = static_cast<float>(lineGap) * bakeScale;

  const std::vector<int> codepoints = buildAtlasCodepointList();
  atlas.glyphs.assign(codepoints.size(), stbtt_packedchar{});
  atlas.codepointToGlyphIndex.clear();

  std::vector<unsigned char> alpha(
      static_cast<std::size_t>(atlas.atlasWidth) * static_cast<std::size_t>(atlas.atlasHeight),
      0U);

  stbtt_pack_context packContext{};
  const int packBegin = stbtt_PackBegin(
      &packContext,
      alpha.data(),
      static_cast<int>(atlas.atlasWidth),
      static_cast<int>(atlas.atlasHeight),
      0,
      1,
      nullptr);
  if (packBegin == 0) {
    return false;
  }

  stbtt_pack_range range{};
  range.font_size = atlas.bakePixelHeight;
  range.first_unicode_codepoint_in_range = 0;
  range.array_of_unicode_codepoints = const_cast<int*>(codepoints.data());
  range.num_chars = static_cast<int>(codepoints.size());
  range.chardata_for_range = atlas.glyphs.data();

  const int packed = stbtt_PackFontRanges(
      &packContext,
      fontBytes.data(),
      0,
      &range,
      1);
  stbtt_PackEnd(&packContext);

  if (packed == 0) {
    return false;
  }

  for (std::size_t i = 0; i < codepoints.size(); ++i) {
    atlas.codepointToGlyphIndex.emplace(static_cast<std::uint32_t>(codepoints[i]), static_cast<int>(i));
  }

  volt::io::DecodedImage atlasImage{};
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
  atlas.valid = volt::io::encodeImageFile(atlasPath, atlasImage, volt::io::ImageEncodeFormat::kPng);
  return atlas.valid;
}

void appendTextCommand(const UiTextCommand& text, UiMeshData* mesh) {
  if (mesh == nullptr || text.text.empty() || text.bounds.width <= 0.0F || text.bounds.height <= 0.0F) {
    return;
  }

  if (!ensureFontAtlas()) {
    return;
  }

  const FontAtlasData& atlas = fontAtlas();

  const std::uint32_t firstIndex = static_cast<std::uint32_t>(mesh->indices.size());

  const float scale = std::max(0.01F, text.fontSizePx / atlas.bakePixelHeight);
  const float lineHeight = std::max(1.0F, (atlas.ascentPx - atlas.descentPx + atlas.lineGapPx) * scale);
  const float baselineY =
      text.bounds.y + std::max(0.0F, (text.bounds.height - lineHeight) * 0.5F) + atlas.ascentPx * scale;

  float x = 0.0F;
  float y = 0.0F;
  struct GlyphQuad {
    stbtt_aligned_quad quad{};
  };
  std::vector<GlyphQuad> quads;
  const std::vector<std::uint32_t> codepoints = decodeUtf8ToCodepoints(text.text);
  quads.reserve(codepoints.size());

  for (const std::uint32_t cp : codepoints) {
    if (cp == static_cast<std::uint32_t>('\n')) {
      x = 0.0F;
      y += (atlas.ascentPx - atlas.descentPx + atlas.lineGapPx);
      continue;
    }

    const int glyphIndex = atlasGlyphIndexForCodepoint(atlas, cp);
    if (glyphIndex < 0) {
      continue;
    }

    stbtt_aligned_quad q{};
    stbtt_GetPackedQuad(
        atlas.glyphs.data(),
        static_cast<int>(atlas.atlasWidth),
        static_cast<int>(atlas.atlasHeight),
        glyphIndex,
        &x,
        &y,
        &q,
        0);
    quads.push_back(GlyphQuad{q});
  }

  if (quads.empty()) {
    return;
  }

  const float baseX = text.bounds.x;
  const float baseY = baselineY;
  const float right = text.bounds.x + text.bounds.width;
  const float bottom = text.bounds.y + text.bounds.height;

  for (const GlyphQuad& glyph : quads) {
    const auto& q = glyph.quad;

    Rect quadBounds{
        baseX + q.x0 * scale,
        baseY + q.y0 * scale,
        (q.x1 - q.x0) * scale,
        (q.y1 - q.y0) * scale,
    };

    if (quadBounds.x >= right || quadBounds.y >= bottom) {
      continue;
    }
    if (quadBounds.x + quadBounds.width <= text.bounds.x ||
        quadBounds.y + quadBounds.height <= text.bounds.y) {
      continue;
    }

    appendQuadGeometryUv(
        quadBounds,
        text.color,
        q.s0,
        q.t0,
        q.s1,
        q.t1,
        mesh);
  }

  const std::uint32_t indexCount = static_cast<std::uint32_t>(mesh->indices.size()) - firstIndex;
  if (indexCount > 0U) {
    mesh->batches.push_back(UiMeshBatch{
        text.widgetId,
        firstIndex,
        indexCount,
        text.bounds,
        atlas.textureKey,
    });
  }
}

}  // namespace

UiMeshData buildUiMesh(const std::vector<UiRenderCommand>& commands) {
  UiMeshData mesh{};
  mesh.vertices.reserve(commands.size() * 4U);
  mesh.indices.reserve(commands.size() * 6U);
  mesh.batches.reserve(commands.size());

  for (const UiRenderCommand& command : commands) {
    std::visit(
        [&](const auto& typed) {
          using T = std::decay_t<decltype(typed)>;
          if constexpr (std::is_same_v<T, UiTextCommand>) {
            appendTextCommand(typed, &mesh);
          } else if constexpr (std::is_same_v<T, UiRectCommand>) {
            appendQuad(typed.bounds, typed.fill, typed.widgetId, "__white", &mesh);
          } else if constexpr (std::is_same_v<T, UiImageCommand>) {
            appendQuad(typed.bounds, typed.tint, typed.widgetId, typed.imageKey, &mesh);
          } else if constexpr (std::is_same_v<T, UiIconCommand>) {
            appendQuad(typed.bounds, typed.tint, typed.widgetId, typed.iconKey, &mesh);
          } else if constexpr (std::is_same_v<T, UiChartScaffoldCommand>) {
            appendQuad(typed.bounds, Color{0.15F, 0.20F, 0.28F, 1.0F}, typed.widgetId, "__white", &mesh);
          } else if constexpr (std::is_same_v<T, UiSchematicScaffoldCommand>) {
            appendQuad(typed.bounds, Color{0.12F, 0.16F, 0.20F, 1.0F}, typed.widgetId, "__white", &mesh);
          }
        },
        command);
  }

  return mesh;
}

}  // namespace volt::ui
