#include "volt/io/assets/Font.hpp"

#include "SfntFontAtlas.hpp"

#include "volt/io/assets/Manifest.hpp"
#include "volt/io/image/ImageDecoder.hpp"
#include "volt/io/image/ImageEncoder.hpp"
#include "volt/io/text/Utf.hpp"

#include "volt/core/Logging.hpp"

#include <chrono>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string_view>
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
  std::uint64_t revision{0U};
  bool gpuAtlasEnabled{false};
  bool sdfEnabled{false};
  bool msdfEnabled{false};
  float sdfSpreadPx{0.0F};
  float sdfEdge{0.5F};
  float sdfAaStrength{0.35F};
  float msdfConfidenceLow{0.01F};
  float msdfConfidenceHigh{0.07F};
  float subpixelBlendStrength{0.85F};
  float smallTextSharpenStrength{0.28F};
  float bakePixelHeight{32.0F};
  std::uint32_t atlasWidth{0};
  std::uint32_t atlasHeight{0};
  std::vector<PackedGlyph> glyphs{};
  std::vector<FontVectorGlyph> vectorGlyphs{};
  std::vector<FontVectorCurve> vectorCurves{};
  std::unordered_map<char32_t, int> codepointToGlyphIndex{};
  std::unordered_map<std::uint64_t, float> kerningPairsPx{};
  char32_t fallbackCodepoint{U'?'};
  float ascentPx{0.0F};
  float descentPx{0.0F};
  float lineGapPx{0.0F};
  std::string textureKey{"image:ui-font-default"};
  FontGpuAtlas gpuAtlas{};
};

FontAtlasData& fontAtlas() {
  static FontAtlasData atlas;
  return atlas;
}

struct FontRuntimeTuningState {
  bool atlasOverrideActive{false};
  FontAtlasBuildTuning atlasBuild{};
  bool renderOverrideActive{false};
  FontRenderTuning render{};
};

FontRuntimeTuningState& fontRuntimeTuningState() {
  static FontRuntimeTuningState state;
  return state;
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

constexpr std::uint64_t makeKerningPairKey(int leftGlyphIndex, int rightGlyphIndex) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(leftGlyphIndex)) << 32U) |
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(rightGlyphIndex));
}

void clearFontAtlasBuildProducts(FontAtlasData& atlas) {
  atlas.valid = false;
  atlas.gpuAtlasEnabled = false;
  atlas.glyphs.clear();
  atlas.vectorGlyphs.clear();
  atlas.vectorCurves.clear();
  atlas.codepointToGlyphIndex.clear();
  atlas.kerningPairsPx.clear();
  atlas.gpuAtlas = FontGpuAtlas{};
}

struct FontManifestObserverState {
  bool subscribed{false};
  std::uint64_t subscriptionId{0U};
  bool dirty{true};
  std::chrono::steady_clock::time_point lastRefreshPoll{};
};

constexpr auto kFontManifestPollInterval = std::chrono::milliseconds(100);

FontManifestObserverState& fontManifestObserverState() {
  static FontManifestObserverState state;
  return state;
}

void ensureFontManifestSubscription() {
  FontManifestObserverState& state = fontManifestObserverState();
  if (state.subscribed) {
    return;
  }

  KeyValueManifest& manifest = manifestService();
  state.subscriptionId = manifest.subscribe(
      [](const KeyValueManifest& updatedManifest) {
        (void)updatedManifest;
        FontManifestObserverState& observerState = fontManifestObserverState();
        observerState.dirty = true;
      });
  state.subscribed = true;
}

bool equalsAsciiCaseInsensitive(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  for (std::size_t i = 0U; i < lhs.size(); ++i) {
    const unsigned char l = static_cast<unsigned char>(lhs[i]);
    const unsigned char r = static_cast<unsigned char>(rhs[i]);
    if (std::tolower(l) != std::tolower(r)) {
      return false;
    }
  }

  return true;
}

const char* rasterModeLabel(detail::SfntAtlasRasterMode mode) {
  switch (mode) {
    case detail::SfntAtlasRasterMode::kSignedDistanceField:
      return "sdf";
    case detail::SfntAtlasRasterMode::kMultiChannelSignedDistanceField:
      return "msdf";
    case detail::SfntAtlasRasterMode::kCoverage:
    default:
      return "coverage";
  }
}

constexpr float kDefaultSdfAaStrength = 0.35F;
constexpr float kDefaultSdfEdge = 0.5F;
constexpr float kDefaultMsdfConfidenceLow = 0.009F;
constexpr float kDefaultMsdfConfidenceHigh = 0.128F;
constexpr float kDefaultSubpixelBlendStrength = 0.123F;
constexpr float kDefaultSmallTextSharpenStrength = 0.229F;
constexpr FontSamplingMode kDefaultFontSamplingMode = FontSamplingMode::kMultiChannelSignedDistanceField;

FontRasterMode toPublicRasterMode(detail::SfntAtlasRasterMode mode) {
  switch (mode) {
    case detail::SfntAtlasRasterMode::kSignedDistanceField:
      return FontRasterMode::kSignedDistanceField;
    case detail::SfntAtlasRasterMode::kMultiChannelSignedDistanceField:
      return FontRasterMode::kMultiChannelSignedDistanceField;
    case detail::SfntAtlasRasterMode::kCoverage:
    default:
      return FontRasterMode::kCoverage;
  }
}

detail::SfntAtlasRasterMode toInternalRasterMode(FontRasterMode mode) {
  switch (mode) {
    case FontRasterMode::kSignedDistanceField:
      return detail::SfntAtlasRasterMode::kSignedDistanceField;
    case FontRasterMode::kMultiChannelSignedDistanceField:
      return detail::SfntAtlasRasterMode::kMultiChannelSignedDistanceField;
    case FontRasterMode::kCoverage:
    default:
      return detail::SfntAtlasRasterMode::kCoverage;
  }
}

void invalidateDefaultFontAtlasBuild() {
  FontAtlasData& atlas = fontAtlas();
  atlas.initialized = false;
  clearFontAtlasBuildProducts(atlas);
  fontManifestObserverState().dirty = true;
}

const std::filesystem::path& embeddedDefaultFontPath() {
  static const std::filesystem::path embeddedPath =
      std::filesystem::path("assets") / "fonts" / "DefaultFont.ttf";
  return embeddedPath;
}

std::optional<std::filesystem::path> resolveManifestFontPath() {
  auto& manifest = manifestService();
  manifest.refresh(false);
  if (manifest.isDisabled()) {
    return std::nullopt;
  }

  const auto manifestPath = manifest.resolvedPathFor("font");
  if (!manifestPath.has_value()) {
    return std::nullopt;
  }

  std::error_code ec;
  if (!std::filesystem::exists(*manifestPath, ec) || ec) {
    return std::nullopt;
  }

  return std::filesystem::path(*manifestPath);
}

detail::SfntAtlasBuildOptions resolveSfntBuildOptions() {
  detail::SfntAtlasBuildOptions options{};

  auto& manifest = manifestService();
  manifest.refresh(false);
  if (manifest.isDisabled()) {
    return options;
  }

  const auto modeValue = manifest.findString("font-render-mode");
  if (modeValue.has_value()) {
    if (equalsAsciiCaseInsensitive(*modeValue, "sdf")) {
      options.rasterMode = detail::SfntAtlasRasterMode::kSignedDistanceField;
    } else if (equalsAsciiCaseInsensitive(*modeValue, "msdf")) {
      options.rasterMode = detail::SfntAtlasRasterMode::kMultiChannelSignedDistanceField;
    }
  }

  const auto spreadValue = manifest.findNumber("font-sdf-spread-px");
  if (spreadValue.has_value() && *spreadValue > 0.0) {
    options.sdfSpreadPx = std::clamp(static_cast<int>(std::lround(*spreadValue)), 2, 64);
  }

  const FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  if (runtimeTuning.atlasOverrideActive) {
    options.rasterMode = toInternalRasterMode(runtimeTuning.atlasBuild.rasterMode);
    if (runtimeTuning.atlasBuild.sdfSpreadPx > 0.0F) {
      options.sdfSpreadPx = std::clamp(static_cast<int>(std::lround(runtimeTuning.atlasBuild.sdfSpreadPx)), 2, 64);
    }
  }

  return options;
}

float resolveSdfAaStrength() {
  const FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  if (runtimeTuning.renderOverrideActive && runtimeTuning.render.sdfAaStrength > 0.0F) {
    return std::clamp(runtimeTuning.render.sdfAaStrength, 0.1F, 0.6F);
  }

  auto& manifest = manifestService();
  manifest.refresh(false);
  if (manifest.isDisabled()) {
    return kDefaultSdfAaStrength;
  }

  const auto aaValue = manifest.findNumber("font-sdf-aa-strength");
  if (!aaValue.has_value() || *aaValue <= 0.0) {
    return kDefaultSdfAaStrength;
  }

  return std::clamp(static_cast<float>(*aaValue), 0.1F, 0.6F);
}

float resolveSdfEdge() {
  const FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  if (runtimeTuning.renderOverrideActive && runtimeTuning.render.sdfEdge > 0.0F) {
    return std::clamp(runtimeTuning.render.sdfEdge, 0.35F, 0.65F);
  }

  auto& manifest = manifestService();
  manifest.refresh(false);
  if (manifest.isDisabled()) {
    return kDefaultSdfEdge;
  }

  const auto edgeValue = manifest.findNumber("font-sdf-edge");
  if (!edgeValue.has_value() || *edgeValue <= 0.0) {
    return kDefaultSdfEdge;
  }

  return std::clamp(static_cast<float>(*edgeValue), 0.35F, 0.65F);
}

FontSamplingMode resolveFontSamplingMode() {
  const FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  if (runtimeTuning.renderOverrideActive) {
    return runtimeTuning.render.samplingMode;
  }
  return kDefaultFontSamplingMode;
}

float resolveMsdfConfidenceLow() {
  const FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  if (runtimeTuning.renderOverrideActive && runtimeTuning.render.msdfConfidenceLow >= 0.0F) {
    return std::clamp(runtimeTuning.render.msdfConfidenceLow, 0.0F, 0.2F);
  }
  return kDefaultMsdfConfidenceLow;
}

float resolveMsdfConfidenceHigh() {
  const FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  if (runtimeTuning.renderOverrideActive && runtimeTuning.render.msdfConfidenceHigh >= 0.0F) {
    return std::clamp(runtimeTuning.render.msdfConfidenceHigh, 0.0F, 0.25F);
  }
  return kDefaultMsdfConfidenceHigh;
}

float resolveSubpixelBlendStrength() {
  const FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  if (runtimeTuning.renderOverrideActive && runtimeTuning.render.subpixelBlendStrength >= 0.0F) {
    return std::clamp(runtimeTuning.render.subpixelBlendStrength, 0.0F, 1.0F);
  }
  return kDefaultSubpixelBlendStrength;
}

float resolveSmallTextSharpenStrength() {
  const FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  if (runtimeTuning.renderOverrideActive && runtimeTuning.render.smallTextSharpenStrength >= 0.0F) {
    return std::clamp(runtimeTuning.render.smallTextSharpenStrength, 0.0F, 1.0F);
  }
  return kDefaultSmallTextSharpenStrength;
}

std::filesystem::path resolveDefaultFontAtlasOutputPath() {
  auto& manifest = manifestService();
  manifest.refresh(false);
  if (!manifest.isDisabled()) {
    if (const auto resolved = manifest.resolvedPathFor("ui-font-default"); resolved.has_value()) {
      return std::filesystem::path(*resolved).lexically_normal();
    }
  }

  return (std::filesystem::path("assets") / "images" / "ui-font-default.png").lexically_normal();
}

bool persistAtlasImage(const FontAtlasData& atlas, const std::vector<std::uint8_t>& rgba) {
  if (atlas.atlasWidth == 0U || atlas.atlasHeight == 0U) {
    return false;
  }

  if (rgba.size() < static_cast<std::size_t>(atlas.atlasWidth) * static_cast<std::size_t>(atlas.atlasHeight) * 4U) {
    return false;
  }

  RawImage atlasImage{};
  atlasImage.width = atlas.atlasWidth;
  atlasImage.height = atlas.atlasHeight;
  atlasImage.rgba = rgba;

  const std::filesystem::path atlasPath = resolveDefaultFontAtlasOutputPath();
  if (atlasPath.has_parent_path()) {
    std::error_code mkErr;
    std::filesystem::create_directories(atlasPath.parent_path(), mkErr);
  }

  std::error_code ec;
  if (std::filesystem::exists(atlasPath, ec) && !ec) {
    RawImage existing{};
    if (decodeImageFile(atlasPath, existing) &&
        existing.width == atlasImage.width &&
        existing.height == atlasImage.height &&
        existing.rgba == atlasImage.rgba) {
      return true;
    }
  }

  return encodeImageFile(atlasPath, atlasImage, ImageEncodeFormat::kPng);
}

bool buildSfntDefaultFontAtlas(FontAtlasData& atlas) {
  const detail::SfntAtlasBuildOptions buildOptions = resolveSfntBuildOptions();
  const float resolvedSdfEdge = resolveSdfEdge();
  const float resolvedSdfAaStrength = resolveSdfAaStrength();
  const float resolvedMsdfConfidenceLow = resolveMsdfConfidenceLow();
  const float resolvedMsdfConfidenceHigh = resolveMsdfConfidenceHigh();
  const float resolvedSubpixelBlendStrength = resolveSubpixelBlendStrength();
  const float resolvedSmallTextSharpenStrength = resolveSmallTextSharpenStrength();
  const std::vector<int> codepoints = buildAtlasCodepointList();
  const bool wantsGpuAtlas = buildOptions.rasterMode != detail::SfntAtlasRasterMode::kCoverage;

  std::vector<std::filesystem::path> candidates;
  if (const auto manifestPath = resolveManifestFontPath(); manifestPath.has_value()) {
    candidates.push_back(*manifestPath);
  }
  candidates.push_back(embeddedDefaultFontPath());

  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

  if (candidates.empty()) {
    VOLT_LOG_ERROR_CAT(
        volt::core::logging::Category::kIO,
        "No default font candidates are available.");
    return false;
  }

  VOLT_LOG_DEBUG_CAT(
      volt::core::logging::Category::kIO,
      "Default font atlas build settings: mode=",
      rasterModeLabel(buildOptions.rasterMode),
      " spreadPx=",
      buildOptions.sdfSpreadPx,
      " edge=",
      resolvedSdfEdge,
      " aa=",
      resolvedSdfAaStrength);

  const float bakePixelHeight =
      (buildOptions.rasterMode == detail::SfntAtlasRasterMode::kSignedDistanceField ||
       buildOptions.rasterMode == detail::SfntAtlasRasterMode::kMultiChannelSignedDistanceField)
        ? 96.0F
        : 48.0F;

  atlas.gpuAtlasEnabled = false;
  atlas.gpuAtlas = FontGpuAtlas{};

  detail::SfntAtlasResult rasterResult{};
  detail::SfntGpuAtlasResult gpuResult{};
  detail::SfntGpuAtlasResult vectorResult{};
  std::filesystem::path activeFontPath;
  bool built = false;
  bool builtWithGpuAtlas = false;
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec) || ec) {
      continue;
    }

    if (wantsGpuAtlas) {
      gpuResult = detail::SfntGpuAtlasResult{};
      if (detail::buildSfntGpuAtlasFromFile(candidate, bakePixelHeight, codepoints, buildOptions, gpuResult) &&
          gpuResult.success) {
        activeFontPath = candidate;
        built = true;
        builtWithGpuAtlas = true;
        break;
      }

      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "Default font GPU atlas build failed for '",
          candidate.string(),
          "': ",
          gpuResult.error,
          "; falling back to CPU atlas generation.");
    }

    rasterResult = detail::SfntAtlasResult{};
    if (detail::buildSfntAtlasFromFile(candidate, bakePixelHeight, codepoints, buildOptions, rasterResult) &&
        rasterResult.success) {
      activeFontPath = candidate;
      built = true;
      builtWithGpuAtlas = false;
      break;
    }

    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "Default font sfnt parse failed for '",
        candidate.string(),
        "': ",
        wantsGpuAtlas ? rasterResult.error : rasterResult.error);
  }

  if (!built) {
    return false;
  }

  atlas.bakePixelHeight = builtWithGpuAtlas ? gpuResult.bakePixelHeight : rasterResult.bakePixelHeight;
  atlas.sdfEnabled =
      buildOptions.rasterMode == detail::SfntAtlasRasterMode::kSignedDistanceField ||
      buildOptions.rasterMode == detail::SfntAtlasRasterMode::kMultiChannelSignedDistanceField;
  atlas.msdfEnabled = buildOptions.rasterMode == detail::SfntAtlasRasterMode::kMultiChannelSignedDistanceField;
  atlas.sdfSpreadPx = atlas.sdfEnabled ? static_cast<float>(buildOptions.sdfSpreadPx) : 0.0F;
  atlas.sdfEdge = atlas.sdfEnabled ? resolvedSdfEdge : 0.5F;
  atlas.sdfAaStrength = atlas.sdfEnabled ? resolvedSdfAaStrength : 1.0F;
  atlas.msdfConfidenceLow = resolvedMsdfConfidenceLow;
  atlas.msdfConfidenceHigh = resolvedMsdfConfidenceHigh;
  atlas.subpixelBlendStrength = resolvedSubpixelBlendStrength;
  atlas.smallTextSharpenStrength = resolvedSmallTextSharpenStrength;
  atlas.ascentPx = builtWithGpuAtlas ? gpuResult.ascentPx : rasterResult.ascentPx;
  atlas.descentPx = builtWithGpuAtlas ? gpuResult.descentPx : rasterResult.descentPx;
  atlas.lineGapPx = builtWithGpuAtlas ? gpuResult.lineGapPx : rasterResult.lineGapPx;
  atlas.atlasWidth = builtWithGpuAtlas ? gpuResult.atlasWidth : rasterResult.atlasWidth;
  atlas.atlasHeight = builtWithGpuAtlas ? gpuResult.atlasHeight : rasterResult.atlasHeight;
  atlas.fallbackCodepoint = U'?';

  const auto& glyphSource = builtWithGpuAtlas ? gpuResult.glyphs : rasterResult.glyphs;
  atlas.glyphs.resize(glyphSource.size());
  for (std::size_t i = 0U; i < glyphSource.size(); ++i) {
    const auto& in = glyphSource[i];
    PackedGlyph out{};
    out.xOffset = in.xOffset;
    out.yOffset = in.yOffset;
    out.width = in.width;
    out.height = in.height;
    out.s0 = in.s0;
    out.t0 = in.t0;
    out.s1 = in.s1;
    out.t1 = in.t1;
    out.xAdvance = in.xAdvance;
    atlas.glyphs[i] = out;
  }

  atlas.codepointToGlyphIndex = builtWithGpuAtlas
                                    ? std::move(gpuResult.codepointToGlyphIndex)
                                    : std::move(rasterResult.codepointToGlyphIndex);
  atlas.kerningPairsPx = builtWithGpuAtlas
                             ? std::move(gpuResult.kerningPairsPx)
                             : std::move(rasterResult.kerningPairsPx);

  if (builtWithGpuAtlas) {
    vectorResult = gpuResult;
  } else {
    detail::SfntAtlasBuildOptions vectorBuildOptions = buildOptions;
    if (vectorBuildOptions.rasterMode == detail::SfntAtlasRasterMode::kCoverage) {
      vectorBuildOptions.rasterMode = detail::SfntAtlasRasterMode::kSignedDistanceField;
      vectorBuildOptions.sdfSpreadPx = std::max(vectorBuildOptions.sdfSpreadPx, 8);
    }

    if (!detail::buildSfntGpuAtlasFromFile(activeFontPath, bakePixelHeight, codepoints, vectorBuildOptions, vectorResult) ||
        !vectorResult.success) {
      VOLT_LOG_ERROR_CAT(
          volt::core::logging::Category::kIO,
          "Failed to build default vector glyph data from '",
          activeFontPath.string(),
          "': ",
          vectorResult.error);
      return false;
    }
  }

  atlas.vectorGlyphs.resize(vectorResult.glyphs.size());
  for (std::size_t i = 0U; i < vectorResult.glyphs.size(); ++i) {
    const auto& in = vectorResult.glyphs[i];
    FontVectorGlyph out{};
    out.xOffset = in.xOffset;
    out.yOffset = in.yOffset;
    out.width = in.width;
    out.height = in.height;
    out.xAdvance = in.xAdvance;
    atlas.vectorGlyphs[i] = out;
  }

  for (const auto& job : vectorResult.jobs) {
    const std::int32_t glyphIndex = job.pad0;
    if (glyphIndex < 0 || glyphIndex >= static_cast<std::int32_t>(atlas.vectorGlyphs.size())) {
      continue;
    }

    FontVectorGlyph& glyph = atlas.vectorGlyphs[static_cast<std::size_t>(glyphIndex)];
    glyph.curveOffset = static_cast<std::uint32_t>(std::max(0, job.curveOffset));
    glyph.curveCount = static_cast<std::uint32_t>(std::max(0, job.curveCount));
  }

  atlas.vectorCurves.resize(vectorResult.curves.size());
  for (std::size_t i = 0U; i < vectorResult.curves.size(); ++i) {
    const auto& in = vectorResult.curves[i];
    atlas.vectorCurves[i] = FontVectorCurve{
        in.p0x,
        in.p0y,
        in.p1x,
        in.p1y,
        in.p2x,
        in.p2y,
        in.p3x,
        in.p3y,
        in.type,
    };
  }

  if (builtWithGpuAtlas) {
    atlas.gpuAtlasEnabled = true;
    atlas.gpuAtlas.atlasWidth = gpuResult.atlasWidth;
    atlas.gpuAtlas.atlasHeight = gpuResult.atlasHeight;
    atlas.gpuAtlas.maxGlyphWidth = gpuResult.maxGlyphWidth;
    atlas.gpuAtlas.maxGlyphHeight = gpuResult.maxGlyphHeight;
    atlas.gpuAtlas.sdfSpreadPx = atlas.sdfSpreadPx;
    atlas.gpuAtlas.msdfEnabled = atlas.msdfEnabled;
    atlas.gpuAtlas.curves.resize(gpuResult.curves.size());
    for (std::size_t i = 0U; i < gpuResult.curves.size(); ++i) {
      const auto& in = gpuResult.curves[i];
      atlas.gpuAtlas.curves[i] = FontGpuCurveSegment{
          in.p0x,
          in.p0y,
          in.p1x,
          in.p1y,
          in.p2x,
          in.p2y,
          in.p3x,
          in.p3y,
          in.type,
          in.channelMask,
          in.contourSign,
          in.pad2,
      };
    }
    atlas.gpuAtlas.jobs.resize(gpuResult.jobs.size());
    for (std::size_t i = 0U; i < gpuResult.jobs.size(); ++i) {
      const auto& in = gpuResult.jobs[i];
      atlas.gpuAtlas.jobs[i] = FontGpuGlyphJob{
          in.atlasX,
          in.atlasY,
          in.glyphWidth,
          in.glyphHeight,
          in.curveOffset,
          in.curveCount,
          in.pad0,
          in.pad1,
      };
    }
  } else if (!persistAtlasImage(atlas, rasterResult.rgba)) {
    return false;
  }

  if ((builtWithGpuAtlas ? gpuResult.partial : rasterResult.partial)) {
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "Font atlas built from '",
      activeFontPath.string(),
        "' with partial glyph coverage");
  }

  if (buildOptions.rasterMode == detail::SfntAtlasRasterMode::kSignedDistanceField ||
      buildOptions.rasterMode == detail::SfntAtlasRasterMode::kMultiChannelSignedDistanceField) {
    VOLT_LOG_INFO_CAT(
        volt::core::logging::Category::kIO,
        builtWithGpuAtlas ? "Built default GPU font atlas in " : "Built default CPU font atlas in ",
        buildOptions.rasterMode == detail::SfntAtlasRasterMode::kMultiChannelSignedDistanceField ? "MSDF" : "SDF",
        " mode with spread ",
        buildOptions.sdfSpreadPx,
      " px, edge ",
      atlas.sdfEdge,
      ", aa ",
      atlas.sdfAaStrength);
  }

  return true;
}

}  // namespace

bool ensureDefaultFontAtlas() {
  ensureFontManifestSubscription();

  FontAtlasData& atlas = fontAtlas();
  FontManifestObserverState& observerState = fontManifestObserverState();
  const auto now = std::chrono::steady_clock::now();
  const bool shouldPollManifest =
      !atlas.initialized ||
      observerState.dirty ||
      observerState.lastRefreshPoll == std::chrono::steady_clock::time_point{} ||
      (now - observerState.lastRefreshPoll) >= kFontManifestPollInterval;

  if (shouldPollManifest) {
    KeyValueManifest& manifest = manifestService();
    manifest.refresh(false);
    observerState.lastRefreshPoll = now;
  }

  const bool rebuildTriggered = atlas.initialized && observerState.dirty;

  if (rebuildTriggered) {
    VOLT_LOG_INFO_CAT(
        volt::core::logging::Category::kIO,
        "Manifest hot reload detected for default font atlas; rebuilding now.");
    atlas.initialized = false;
    clearFontAtlasBuildProducts(atlas);
  }

  if (atlas.initialized) {
    return atlas.valid;
  }
  atlas.initialized = true;

  if (buildSfntDefaultFontAtlas(atlas)) {
    atlas.valid = true;
    ++atlas.revision;
    observerState.dirty = false;
    if (rebuildTriggered) {
      VOLT_LOG_INFO_CAT(
          volt::core::logging::Category::kIO,
          "Default font atlas rebuild completed successfully.");
    }
    return true;
  }

  VOLT_LOG_ERROR_CAT(
      volt::core::logging::Category::kIO,
      "Failed to build default sfnt font atlas from manifest and embedded fallback font.");

  clearFontAtlasBuildProducts(atlas);
  observerState.dirty = false;
  if (rebuildTriggered) {
    VOLT_LOG_ERROR_CAT(
        volt::core::logging::Category::kIO,
        "Default font atlas rebuild failed; no fallback bitmap atlas is available.");
  }
  return atlas.valid;
}

bool defaultFontMetrics(FontMetrics& outMetrics) {
  if (!ensureDefaultFontAtlas()) {
    return false;
  }

  const FontAtlasData& atlas = fontAtlas();
  const FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  outMetrics.bakePixelHeight = atlas.bakePixelHeight;
  outMetrics.ascentPx = atlas.ascentPx;
  outMetrics.descentPx = atlas.descentPx;
  outMetrics.lineGapPx = atlas.lineGapPx;
  outMetrics.sdfEnabled = atlas.sdfEnabled;
  outMetrics.msdfEnabled = atlas.msdfEnabled;
  outMetrics.sdfSpreadPx = atlas.sdfSpreadPx;
  outMetrics.sdfEdge = atlas.sdfEdge;
  outMetrics.sdfAaStrength = atlas.sdfAaStrength;
  outMetrics.samplingMode = resolveFontSamplingMode();
  outMetrics.msdfConfidenceLow = atlas.msdfConfidenceLow;
  outMetrics.msdfConfidenceHigh = atlas.msdfConfidenceHigh;
  outMetrics.subpixelBlendStrength = atlas.subpixelBlendStrength;
  outMetrics.smallTextSharpenStrength = atlas.smallTextSharpenStrength;

  if (runtimeTuning.renderOverrideActive) {
    outMetrics.sdfEdge = runtimeTuning.render.sdfEdge;
    outMetrics.sdfAaStrength = runtimeTuning.render.sdfAaStrength;
    outMetrics.samplingMode = runtimeTuning.render.samplingMode;
    outMetrics.msdfConfidenceLow = runtimeTuning.render.msdfConfidenceLow;
    outMetrics.msdfConfidenceHigh = runtimeTuning.render.msdfConfidenceHigh;
    outMetrics.subpixelBlendStrength = runtimeTuning.render.subpixelBlendStrength;
    outMetrics.smallTextSharpenStrength = runtimeTuning.render.smallTextSharpenStrength;
  }
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

float defaultFontKerningAdvance(int leftGlyphIndex, int rightGlyphIndex) {
  if (!ensureDefaultFontAtlas()) {
    return 0.0F;
  }

  const FontAtlasData& atlas = fontAtlas();
  if (leftGlyphIndex < 0 || rightGlyphIndex < 0 ||
      leftGlyphIndex >= static_cast<int>(atlas.glyphs.size()) ||
      rightGlyphIndex >= static_cast<int>(atlas.glyphs.size())) {
    return 0.0F;
  }

  const auto it = atlas.kerningPairsPx.find(makeKerningPairKey(leftGlyphIndex, rightGlyphIndex));
  if (it == atlas.kerningPairsPx.end()) {
    return 0.0F;
  }

  return it->second;
}

bool defaultFontVectorGlyph(int glyphIndex, FontVectorGlyph& outGlyph) {
  if (!ensureDefaultFontAtlas()) {
    return false;
  }

  const FontAtlasData& atlas = fontAtlas();
  if (glyphIndex < 0 || glyphIndex >= static_cast<int>(atlas.vectorGlyphs.size())) {
    return false;
  }

  outGlyph = atlas.vectorGlyphs[static_cast<std::size_t>(glyphIndex)];
  return true;
}

const std::vector<FontVectorCurve>& defaultFontVectorCurves() {
  static const std::vector<FontVectorCurve> emptyCurves{};
  if (!ensureDefaultFontAtlas()) {
    return emptyCurves;
  }
  return fontAtlas().vectorCurves;
}

bool defaultFontGpuAtlas(FontGpuAtlas& outAtlas) {
  if (!ensureDefaultFontAtlas()) {
    return false;
  }

  const FontAtlasData& atlas = fontAtlas();
  if (!atlas.gpuAtlasEnabled) {
    return false;
  }

  outAtlas = atlas.gpuAtlas;
  return true;
}

const std::string& defaultFontTextureKey() {
  return fontAtlas().textureKey;
}

std::uint64_t defaultFontAtlasRevision() {
  return fontAtlas().revision;
}

bool defaultFontAtlasBuildTuning(FontAtlasBuildTuning& outTuning) {
  const detail::SfntAtlasBuildOptions options = resolveSfntBuildOptions();
  outTuning.rasterMode = toPublicRasterMode(options.rasterMode);
  outTuning.sdfSpreadPx =
      options.rasterMode == detail::SfntAtlasRasterMode::kCoverage ? 0.0F : static_cast<float>(options.sdfSpreadPx);
  return true;
}

bool defaultFontRenderTuning(FontRenderTuning& outTuning) {
  FontMetrics metrics{};
  if (!defaultFontMetrics(metrics)) {
    return false;
  }

  outTuning.sdfEdge = metrics.sdfEdge;
  outTuning.sdfAaStrength = metrics.sdfAaStrength;
  outTuning.samplingMode = metrics.samplingMode;
  outTuning.msdfConfidenceLow = metrics.msdfConfidenceLow;
  outTuning.msdfConfidenceHigh = metrics.msdfConfidenceHigh;
  outTuning.subpixelBlendStrength = metrics.subpixelBlendStrength;
  outTuning.smallTextSharpenStrength = metrics.smallTextSharpenStrength;
  return true;
}

void setDefaultFontAtlasBuildTuningOverride(const FontAtlasBuildTuning& tuning) {
  FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  const FontAtlasBuildTuning clampedTuning{
      tuning.rasterMode,
      tuning.sdfSpreadPx > 0.0F ? static_cast<float>(std::clamp(static_cast<int>(std::lround(tuning.sdfSpreadPx)), 2, 64)) : 0.0F,
  };
  if (runtimeTuning.atlasOverrideActive &&
      runtimeTuning.atlasBuild.rasterMode == clampedTuning.rasterMode &&
      std::abs(runtimeTuning.atlasBuild.sdfSpreadPx - clampedTuning.sdfSpreadPx) < 0.001F) {
    return;
  }

  runtimeTuning.atlasOverrideActive = true;
  runtimeTuning.atlasBuild = clampedTuning;
  invalidateDefaultFontAtlasBuild();
}

void clearDefaultFontAtlasBuildTuningOverride() {
  FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  if (!runtimeTuning.atlasOverrideActive) {
    return;
  }

  runtimeTuning.atlasOverrideActive = false;
  runtimeTuning.atlasBuild = FontAtlasBuildTuning{};
  invalidateDefaultFontAtlasBuild();
}

void setDefaultFontRenderTuningOverride(const FontRenderTuning& tuning) {
  FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  const FontRenderTuning clampedTuning{
      std::clamp(tuning.sdfEdge, 0.35F, 0.65F),
      std::clamp(tuning.sdfAaStrength, 0.1F, 0.6F),
    tuning.samplingMode,
      std::clamp(tuning.msdfConfidenceLow, 0.0F, 0.2F),
      std::clamp(tuning.msdfConfidenceHigh, 0.0F, 0.25F),
      std::clamp(tuning.subpixelBlendStrength, 0.0F, 1.0F),
      std::clamp(tuning.smallTextSharpenStrength, 0.0F, 1.0F),
  };

  if (runtimeTuning.renderOverrideActive &&
      std::abs(runtimeTuning.render.sdfEdge - clampedTuning.sdfEdge) < 0.0001F &&
      std::abs(runtimeTuning.render.sdfAaStrength - clampedTuning.sdfAaStrength) < 0.0001F &&
    runtimeTuning.render.samplingMode == clampedTuning.samplingMode &&
      std::abs(runtimeTuning.render.msdfConfidenceLow - clampedTuning.msdfConfidenceLow) < 0.0001F &&
      std::abs(runtimeTuning.render.msdfConfidenceHigh - clampedTuning.msdfConfidenceHigh) < 0.0001F &&
      std::abs(runtimeTuning.render.subpixelBlendStrength - clampedTuning.subpixelBlendStrength) < 0.0001F &&
      std::abs(runtimeTuning.render.smallTextSharpenStrength - clampedTuning.smallTextSharpenStrength) < 0.0001F) {
    return;
  }

  runtimeTuning.renderOverrideActive = true;
  runtimeTuning.render = clampedTuning;
}

void clearDefaultFontRenderTuningOverride() {
  FontRuntimeTuningState& runtimeTuning = fontRuntimeTuningState();
  runtimeTuning.renderOverrideActive = false;
  runtimeTuning.render = FontRenderTuning{};
}

}  // namespace volt::io
