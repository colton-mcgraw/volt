#include "volt/ui/UIMesh.hpp"

#include "volt/io/assets/AssetManager.hpp"
#include "volt/io/text/Utf.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace volt::ui {
namespace {

constexpr float kOpaqueAlphaThreshold = 0.999F;

struct QueuedUiCommand {
  const UiRenderCommand* command{nullptr};
  std::size_t order{0U};
};

[[nodiscard]] float paintDepthForOrder(std::size_t order, std::size_t commandCount) {
  // Keep UI near the front of the depth buffer so it still overlays the scene,
  // while reserving a small visible range for painter-order separation.
  constexpr float kUiDepthSpan = 0.01F;
  const float step = kUiDepthSpan / static_cast<float>(std::max<std::size_t>(1U, commandCount) + 1U);
  return -static_cast<float>(commandCount - order) * step;
}

[[nodiscard]] bool isOpaqueColor(const Color& color) {
  return color.a >= kOpaqueAlphaThreshold;
}

[[nodiscard]] Rect commandClipRect(const UiRenderCommand& command) {
  return std::visit(
      [](const auto& typed) {
        return typed.clipRect;
      },
      command);
}

[[nodiscard]] bool isOpaqueBatchCandidate(const UiRenderCommand& command) {
  return std::visit(
      [](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, UiRectCommand>) {
          return isOpaqueColor(typed.fill);
        } else {
          return false;
        }
      },
      command);
}

[[nodiscard]] bool opaqueCommandLess(const QueuedUiCommand& left, const QueuedUiCommand& right) {
  const Rect leftClip = commandClipRect(*left.command);
  const Rect rightClip = commandClipRect(*right.command);
  if (leftClip.x != rightClip.x) {
    return leftClip.x < rightClip.x;
  }
  if (leftClip.y != rightClip.y) {
    return leftClip.y < rightClip.y;
  }
  if (leftClip.width != rightClip.width) {
    return leftClip.width < rightClip.width;
  }
  if (leftClip.height != rightClip.height) {
    return leftClip.height < rightClip.height;
  }
  return left.order < right.order;
}

void appendQuad(
    const Rect& bounds,
    const Rect& clipRect,
    const Color& color,
    float z,
    std::uint64_t widgetId,
    const std::string& textureKey,
    UiBatchLayer layer,
    UiMeshData* mesh,
    BatchKey* lastBatchKey = nullptr) {
  if (mesh == nullptr || bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return;
  }

  const std::uint32_t baseVertex = static_cast<std::uint32_t>(mesh->vertices.size());
  const std::uint32_t firstIndex = static_cast<std::uint32_t>(mesh->indices.size());

  mesh->vertices.push_back(UiVertex{bounds.x, bounds.y, z, 0.0F, 0.0F, color});
  mesh->vertices.push_back(UiVertex{bounds.x + bounds.width, bounds.y, z, 1.0F, 0.0F, color});
  mesh->vertices.push_back(UiVertex{bounds.x + bounds.width, bounds.y + bounds.height, z, 1.0F, 1.0F, color});
  mesh->vertices.push_back(UiVertex{bounds.x, bounds.y + bounds.height, z, 0.0F, 1.0F, color});

  mesh->indices.push_back(baseVertex + 0U);
  mesh->indices.push_back(baseVertex + 1U);
  mesh->indices.push_back(baseVertex + 2U);
  mesh->indices.push_back(baseVertex + 0U);
  mesh->indices.push_back(baseVertex + 2U);
  mesh->indices.push_back(baseVertex + 3U);

  BatchKey key{
      textureKey,
      layer,
      false, // sdfText
      false, // msdfText
      clipRect
  };
  bool merge = false;
  if (!mesh->batches.empty() && lastBatchKey && *lastBatchKey == key) {
    // Merge with previous batch
    UiMeshBatch& batch = mesh->batches.back();
    batch.indexCount += 6U;
    merge = true;
  }
  if (!merge) {
    mesh->batches.push_back(UiMeshBatch{
        widgetId,
        firstIndex,
        6U,
        clipRect,
        textureKey,
        layer,
        false, false, 0.0F, 0.5F, 0.35F, 0.01F, 0.07F, 0.85F, 0.28F
    });
    if (lastBatchKey) *lastBatchKey = key;
  }
}

void appendQuadGeometryUv(
    const Rect& bounds,
    const Color& color,
  float z,
    float u0,
    float v0,
    float u1,
    float v1,
    UiMeshData* mesh) {
  if (mesh == nullptr || bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return;
  }

  const std::uint32_t baseVertex = static_cast<std::uint32_t>(mesh->vertices.size());

  mesh->vertices.push_back(UiVertex{bounds.x, bounds.y, z, u0, v0, color});
  mesh->vertices.push_back(UiVertex{bounds.x + bounds.width, bounds.y, z, u1, v0, color});
  mesh->vertices.push_back(UiVertex{bounds.x + bounds.width, bounds.y + bounds.height, z, u1, v1, color});
  mesh->vertices.push_back(UiVertex{bounds.x, bounds.y + bounds.height, z, u0, v1, color});

  mesh->indices.push_back(baseVertex + 0U);
  mesh->indices.push_back(baseVertex + 1U);
  mesh->indices.push_back(baseVertex + 2U);
  mesh->indices.push_back(baseVertex + 0U);
  mesh->indices.push_back(baseVertex + 2U);
  mesh->indices.push_back(baseVertex + 3U);
}

void appendTextCommand(const UiTextCommand& text, float z, UiMeshData* mesh, BatchKey* lastBatchKey = nullptr) {
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

  const float scale = std::max(0.01F, text.fontSizePx / metrics.bakePixelHeight);
  const float lineAdvanceUnits = std::max(1.0F, metrics.ascentPx - metrics.descentPx + metrics.lineGapPx);
  const std::string& textureKey = assets.getDefaultFontTextureKey();

  float x = 0.0F;
  float y = 0.0F;
  int previousGlyphIndex = -1;
  struct GlyphQuad {
    volt::io::FontGlyphQuad quad{};
  };
  std::vector<GlyphQuad> quads;
  quads.reserve(codepoints.size());

  const int spaceGlyphIndex = assets.getDefaultFontGlyphIndexForCodepoint(U' ');

  for (const char32_t cp : codepoints) {
    if (cp == U'\r') {
      continue;
    }

    if (cp == U'\n') {
      x = 0.0F;
      y += lineAdvanceUnits;
      previousGlyphIndex = -1;
      continue;
    }

    if (cp == U'\t') {
      if (spaceGlyphIndex >= 0) {
        for (int i = 0; i < 4; ++i) {
          if (previousGlyphIndex >= 0) {
            x += assets.getDefaultFontKerningAdvance(previousGlyphIndex, spaceGlyphIndex);
          }
          volt::io::FontGlyphQuad skip{};
          if (!assets.getDefaultFontPackedQuad(spaceGlyphIndex, x, y, skip)) {
            previousGlyphIndex = -1;
            break;
          }
          previousGlyphIndex = spaceGlyphIndex;
        }
      } else {
        previousGlyphIndex = -1;
      }
      continue;
    }

    const int glyphIndex = assets.getDefaultFontGlyphIndexForCodepoint(cp);
    if (glyphIndex < 0) {
      previousGlyphIndex = -1;
      continue;
    }

    if (previousGlyphIndex >= 0) {
      x += assets.getDefaultFontKerningAdvance(previousGlyphIndex, glyphIndex);
    }

    volt::io::FontGlyphQuad quad{};
    if (!assets.getDefaultFontPackedQuad(glyphIndex, x, y, quad)) {
      previousGlyphIndex = -1;
      continue;
    }

    quads.push_back(GlyphQuad{quad});
    previousGlyphIndex = glyphIndex;
  }

  if (quads.empty()) {
    return;
  }

  float localMinY = quads.front().quad.y0;
  float localMaxY = quads.front().quad.y1;
  for (const GlyphQuad& glyph : quads) {
    localMinY = std::min(localMinY, glyph.quad.y0);
    localMaxY = std::max(localMaxY, glyph.quad.y1);
  }

  const float inkHeight = std::max(0.0F, (localMaxY - localMinY) * scale);
  const float localBaseY = text.bounds.y + std::max(0.0F, (text.bounds.height - inkHeight) * 0.5F) - localMinY * scale;
  BatchKey key{
      textureKey,
      UiBatchLayer::kTransparent,
      metrics.sdfEnabled,
      metrics.msdfEnabled && metrics.samplingMode != volt::io::FontSamplingMode::kSignedDistanceField,
      text.clipRect
  };
  std::uint32_t firstIndex = static_cast<std::uint32_t>(mesh->indices.size());
  bool merge = false;
  if (!mesh->batches.empty() && lastBatchKey && *lastBatchKey == key) {
    // Merge with previous batch
    firstIndex = mesh->batches.back().firstIndex;
    merge = true;
  }
  for (const GlyphQuad& glyph : quads) {
    const volt::io::FontGlyphQuad& quad = glyph.quad;
    Rect quadBounds{
        text.bounds.x + quad.x0 * scale,
        localBaseY + quad.y0 * scale,
        (quad.x1 - quad.x0) * scale,
        (quad.y1 - quad.y0) * scale,
    };

    if (quadBounds.width <= 0.0F || quadBounds.height <= 0.0F) {
      continue;
    }

    // No per-glyph clip for batching: just emit geometry
    const float u0 = quad.s0;
    const float v0 = quad.t0;
    const float u1 = quad.s1;
    const float v1 = quad.t1;
    appendQuadGeometryUv(
        {quadBounds.x, quadBounds.y, quadBounds.width, quadBounds.height},
        text.color,
      z,
        u0,
        v0,
        u1,
        v1,
        mesh);
  }
  std::uint32_t indexCount = static_cast<std::uint32_t>(mesh->indices.size()) - firstIndex;
  if (indexCount > 0U) {
    if (merge) {
      mesh->batches.back().indexCount = indexCount;
    } else {
      mesh->batches.push_back(UiMeshBatch{
          text.widgetId,
          firstIndex,
          indexCount,
          text.clipRect,
          textureKey,
          UiBatchLayer::kTransparent,
          metrics.sdfEnabled,
          metrics.msdfEnabled && metrics.samplingMode != volt::io::FontSamplingMode::kSignedDistanceField,
          std::max(1.0F, metrics.sdfSpreadPx),
          metrics.sdfEdge,
          metrics.sdfAaStrength,
          metrics.msdfConfidenceLow,
          metrics.msdfConfidenceHigh,
          metrics.subpixelBlendStrength,
          metrics.smallTextSharpenStrength,
      });
      if (lastBatchKey) *lastBatchKey = key;
    }
  }
}

}  // namespace

UiMeshData buildUiMesh(const std::vector<UiRenderCommand>& commands, UiMeshBuildOptions options) {
  UiMeshData mesh{};
  mesh.vertices.reserve(commands.size() * 4U);
  mesh.indices.reserve(commands.size() * 6U);
  mesh.batches.reserve(commands.size());

  BatchKey lastBatchKey{};

  auto appendQueuedCommand = [&](const QueuedUiCommand& queued) {
    const float z = paintDepthForOrder(queued.order, commands.size());
    const bool opaque = options.enableOpaqueBatching && isOpaqueBatchCandidate(*queued.command);
    std::visit(
        [&](const auto& typed) {
          using T = std::decay_t<decltype(typed)>;
          if constexpr (std::is_same_v<T, UiTextCommand>) {
            appendTextCommand(typed, z, &mesh, &lastBatchKey);
          } else if constexpr (std::is_same_v<T, UiRectCommand>) {
            appendQuad(
                typed.bounds,
                typed.clipRect,
                typed.fill,
                z,
                typed.widgetId,
                "__white",
                opaque ? UiBatchLayer::kOpaque : UiBatchLayer::kTransparent,
                &mesh,
                &lastBatchKey);
          } else if constexpr (std::is_same_v<T, UiImageCommand>) {
            appendQuad(
                typed.bounds,
                typed.clipRect,
                typed.tint,
                z,
                typed.widgetId,
                typed.imageKey,
                UiBatchLayer::kTransparent,
                &mesh,
                &lastBatchKey);
          } else if constexpr (std::is_same_v<T, UiIconCommand>) {
            appendQuad(
                typed.bounds,
                typed.clipRect,
                typed.tint,
                z,
                typed.widgetId,
                typed.iconKey,
                UiBatchLayer::kTransparent,
                &mesh,
                &lastBatchKey);
          } else if constexpr (std::is_same_v<T, UiChartScaffoldCommand>) {
          } else if constexpr (std::is_same_v<T, UiSchematicScaffoldCommand>) {
          }
        },
        *queued.command);
  };

  if (options.enableOpaqueBatching) {
    std::vector<QueuedUiCommand> opaqueCommands;
    std::vector<QueuedUiCommand> transparentCommands;
    opaqueCommands.reserve(commands.size());
    transparentCommands.reserve(commands.size());

    for (std::size_t commandIndex = 0; commandIndex < commands.size(); ++commandIndex) {
      const UiRenderCommand& command = commands[commandIndex];
      if (isOpaqueBatchCandidate(command)) {
        opaqueCommands.push_back(QueuedUiCommand{&command, commandIndex});
      } else {
        transparentCommands.push_back(QueuedUiCommand{&command, commandIndex});
      }
    }

    std::stable_sort(opaqueCommands.begin(), opaqueCommands.end(), opaqueCommandLess);
    for (const QueuedUiCommand& queued : opaqueCommands) {
      appendQueuedCommand(queued);
    }
    for (const QueuedUiCommand& queued : transparentCommands) {
      appendQueuedCommand(queued);
    }

    return mesh;
  }

  for (std::size_t commandIndex = 0; commandIndex < commands.size(); ++commandIndex) {
    appendQueuedCommand(QueuedUiCommand{&commands[commandIndex], commandIndex});
  }

  return mesh;
}

}  // namespace volt::ui
