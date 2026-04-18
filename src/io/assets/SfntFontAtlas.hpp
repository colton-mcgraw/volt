#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_map>
#include <vector>

namespace volt::io::detail {

enum class SfntAtlasRasterMode {
  kCoverage,
  kSignedDistanceField,
  kMultiChannelSignedDistanceField,
};

struct SfntAtlasBuildOptions {
  SfntAtlasRasterMode rasterMode{SfntAtlasRasterMode::kCoverage};
  int sdfSpreadPx{8};
};

struct SfntGlyphRaster {
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

struct alignas(8) SfntGpuCurveSegment {
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

struct alignas(8) SfntGpuGlyphJob {
  std::int32_t atlasX{0};
  std::int32_t atlasY{0};
  std::int32_t glyphWidth{0};
  std::int32_t glyphHeight{0};
  std::int32_t curveOffset{0};
  std::int32_t curveCount{0};
  std::int32_t pad0{0};
  std::int32_t pad1{0};
};

struct SfntAtlasResult {
  bool success{false};
  bool partial{false};
  std::string error;

  float bakePixelHeight{0.0F};
  float ascentPx{0.0F};
  float descentPx{0.0F};
  float lineGapPx{0.0F};

  std::uint32_t atlasWidth{0};
  std::uint32_t atlasHeight{0};

  std::vector<SfntGlyphRaster> glyphs;
  std::unordered_map<char32_t, int> codepointToGlyphIndex;
  std::vector<std::uint16_t> fontGlyphIndices;
  std::unordered_map<std::uint64_t, float> kerningPairsPx;
  std::vector<std::uint8_t> rgba;
};

struct SfntGpuAtlasResult {
  bool success{false};
  bool partial{false};
  std::string error;

  float bakePixelHeight{0.0F};
  float ascentPx{0.0F};
  float descentPx{0.0F};
  float lineGapPx{0.0F};

  std::uint32_t atlasWidth{0};
  std::uint32_t atlasHeight{0};
  std::uint32_t maxGlyphWidth{0};
  std::uint32_t maxGlyphHeight{0};

  std::vector<SfntGlyphRaster> glyphs;
  std::unordered_map<char32_t, int> codepointToGlyphIndex;
  std::vector<std::uint16_t> fontGlyphIndices;
  std::unordered_map<std::uint64_t, float> kerningPairsPx;
  std::vector<SfntGpuCurveSegment> curves;
  std::vector<SfntGpuGlyphJob> jobs;
};

[[nodiscard]] bool buildSfntAtlasFromFile(const std::filesystem::path& fontPath,
                                          float bakePixelHeight,
                                          const std::vector<int>& codepoints,
                                          const SfntAtlasBuildOptions& buildOptions,
                                          SfntAtlasResult& outResult);

[[nodiscard]] bool buildSfntGpuAtlasFromFile(const std::filesystem::path& fontPath,
                                             float bakePixelHeight,
                                             const std::vector<int>& codepoints,
                                             const SfntAtlasBuildOptions& buildOptions,
                                             SfntGpuAtlasResult& outResult);

[[nodiscard]] std::size_t sfntFontCacheEntryCountForTesting();
[[nodiscard]] std::size_t sfntFontCacheMaxEntriesForTesting();
void clearSfntFontCacheForTesting();

}  // namespace volt::io::detail
