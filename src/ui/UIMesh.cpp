#include "volt/ui/UIMesh.hpp"

#include "volt/io/assets/AssetManager.hpp"
#include "volt/io/text/Utf.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

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

void appendTextCommand(const UiTextCommand& text, UiMeshData* mesh) {
  if (mesh == nullptr || text.text.empty() || text.bounds.width <= 0.0F || text.bounds.height <= 0.0F) {
    return;
  }

  const std::vector<char32_t> codepoints = volt::io::utf::decodeUtf8ToCodepoints(text.text);
  if (codepoints.empty()) {
    return;
  }

  volt::io::AssetManager& assets = volt::io::AssetManager::instance();

  if (!assets.ensureDefaultFontAtlas()) {
    return;
  }

  volt::io::FontMetrics metrics{};
  if (!assets.getDefaultFontMetrics(metrics)) {
    return;
  }

  const std::uint32_t firstIndex = static_cast<std::uint32_t>(mesh->indices.size());

  const float scale = std::max(0.01F, text.fontSizePx / metrics.bakePixelHeight);
  const float lineHeight = std::max(1.0F, (metrics.ascentPx - metrics.descentPx + metrics.lineGapPx) * scale);

  std::uint32_t lineCount = 1U;
  for (const char32_t cp : codepoints) {
    if (cp == U'\n') {
      ++lineCount;
    }
  }

  const float blockHeight = static_cast<float>(lineCount) * lineHeight;
  const float baselineY =
      text.bounds.y + std::max(0.0F, (text.bounds.height - blockHeight) * 0.5F) + metrics.ascentPx * scale;

  float x = 0.0F;
  float y = 0.0F;
  std::vector<volt::io::FontGlyphQuad> quads;
  quads.reserve(codepoints.size());

  const int spaceGlyphIndex = assets.getDefaultFontGlyphIndexForCodepoint(U' ');

  for (const char32_t cp : codepoints) {
    if (cp == U'\r') {
      continue;
    }

    if (cp == U'\n') {
      x = 0.0F;
      y += (metrics.ascentPx - metrics.descentPx + metrics.lineGapPx);
      continue;
    }

    if (cp == U'\t') {
      if (spaceGlyphIndex >= 0) {
        for (int i = 0; i < 4; ++i) {
          volt::io::FontGlyphQuad skip{};
          if (!assets.getDefaultFontPackedQuad(spaceGlyphIndex, x, y, skip)) {
            break;
          }
        }
      }
      continue;
    }

    const int glyphIndex = assets.getDefaultFontGlyphIndexForCodepoint(cp);
    if (glyphIndex < 0) {
      continue;
    }

    volt::io::FontGlyphQuad q{};
    if (!assets.getDefaultFontPackedQuad(glyphIndex, x, y, q)) {
      continue;
    }
    quads.push_back(q);
  }

  if (quads.empty()) {
    return;
  }

  const float baseX = text.bounds.x;
  const float baseY = baselineY;
  const float right = text.bounds.x + text.bounds.width;
  const float bottom = text.bounds.y + text.bounds.height;

  for (const volt::io::FontGlyphQuad& q : quads) {

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
        assets.getDefaultFontTextureKey(),
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
