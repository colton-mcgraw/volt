#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace volt::io {

enum class FontRasterMode {
  kCoverage,
  kSignedDistanceField,
  kMultiChannelSignedDistanceField,
};

enum class FontSamplingMode {
  kAuto,
  kSignedDistanceField,
  kMultiChannelSignedDistanceField,
};

struct FontMetrics {
  float bakePixelHeight{32.0F};
  float ascentPx{0.0F};
  float descentPx{0.0F};
  float lineGapPx{0.0F};
  bool sdfEnabled{false};
  bool msdfEnabled{false};
  float sdfSpreadPx{0.0F};
  float sdfEdge{0.5F};
  float sdfAaStrength{0.35F};
  FontSamplingMode samplingMode{FontSamplingMode::kAuto};
  float msdfConfidenceLow{0.01F};
  float msdfConfidenceHigh{0.07F};
  float subpixelBlendStrength{0.85F};
  float smallTextSharpenStrength{0.28F};
};

struct FontAtlasBuildTuning {
  FontRasterMode rasterMode{FontRasterMode::kCoverage};
  float sdfSpreadPx{0.0F};
};

struct FontRenderTuning {
  float sdfEdge{0.5F};
  float sdfAaStrength{0.35F};
  FontSamplingMode samplingMode{FontSamplingMode::kAuto};
  float msdfConfidenceLow{0.01F};
  float msdfConfidenceHigh{0.07F};
  float subpixelBlendStrength{0.85F};
  float smallTextSharpenStrength{0.28F};
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

struct FontVectorCurve {
  float p0x{0.0F};
  float p0y{0.0F};
  float p1x{0.0F};
  float p1y{0.0F};
  float p2x{0.0F};
  float p2y{0.0F};
  float p3x{0.0F};
  float p3y{0.0F};
  std::int32_t type{0};
};

struct FontVectorGlyph {
  float xOffset{0.0F};
  float yOffset{0.0F};
  float width{0.0F};
  float height{0.0F};
  float xAdvance{0.0F};
  std::uint32_t curveOffset{0};
  std::uint32_t curveCount{0};
};

struct alignas(8) FontGpuCurveSegment {
  float p0x{0.0F};
  float p0y{0.0F};
  float p1x{0.0F};
  float p1y{0.0F};
  float p2x{0.0F};
  float p2y{0.0F};
  float p3x{0.0F};
  float p3y{0.0F};
  std::int32_t type{0};
  std::int32_t channelMask{0x07};
  std::int32_t contourSign{1};
  std::int32_t pad2{0};
};

struct alignas(8) FontGpuGlyphJob {
  std::int32_t atlasX{0};
  std::int32_t atlasY{0};
  std::int32_t glyphWidth{0};
  std::int32_t glyphHeight{0};
  std::int32_t curveOffset{0};
  std::int32_t curveCount{0};
  std::int32_t pad0{0};
  std::int32_t pad1{0};
};

struct FontGpuAtlas {
  std::uint32_t atlasWidth{0};
  std::uint32_t atlasHeight{0};
  std::uint32_t maxGlyphWidth{0};
  std::uint32_t maxGlyphHeight{0};
  float sdfSpreadPx{0.0F};
  bool msdfEnabled{false};
  std::vector<FontGpuCurveSegment> curves;
  std::vector<FontGpuGlyphJob> jobs;
};

[[nodiscard]] bool ensureDefaultFontAtlas();
[[nodiscard]] bool defaultFontMetrics(FontMetrics& outMetrics);
[[nodiscard]] int defaultFontGlyphIndexForCodepoint(char32_t codepoint);
[[nodiscard]] bool defaultFontPackedQuad(int glyphIndex, float& x, float& y, FontGlyphQuad& outQuad);
[[nodiscard]] float defaultFontKerningAdvance(int leftGlyphIndex, int rightGlyphIndex);
[[nodiscard]] bool defaultFontVectorGlyph(int glyphIndex, FontVectorGlyph& outGlyph);
[[nodiscard]] const std::vector<FontVectorCurve>& defaultFontVectorCurves();
[[nodiscard]] bool defaultFontGpuAtlas(FontGpuAtlas& outAtlas);
[[nodiscard]] const std::string& defaultFontTextureKey();
[[nodiscard]] std::uint64_t defaultFontAtlasRevision();
[[nodiscard]] bool defaultFontAtlasBuildTuning(FontAtlasBuildTuning& outTuning);
[[nodiscard]] bool defaultFontRenderTuning(FontRenderTuning& outTuning);
void setDefaultFontAtlasBuildTuningOverride(const FontAtlasBuildTuning& tuning);
void clearDefaultFontAtlasBuildTuningOverride();
void setDefaultFontRenderTuningOverride(const FontRenderTuning& tuning);
void clearDefaultFontRenderTuningOverride();

}  // namespace volt::io
