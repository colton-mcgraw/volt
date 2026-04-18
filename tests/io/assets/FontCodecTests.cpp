#include "volt/io/assets/Font.hpp"
#include "io/assets/SfntFontAtlas.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[font-codec-test] FAIL: " << message << '\n';
    return false;
  }
  return true;
}

constexpr std::uint64_t makeKerningKey(std::uint32_t leftGlyphIndex, std::uint32_t rightGlyphIndex) {
  return (static_cast<std::uint64_t>(leftGlyphIndex) << 32U) |
         static_cast<std::uint64_t>(rightGlyphIndex);
}

std::optional<std::filesystem::path> findWorkspaceRoot() {
  std::error_code ec;
  std::filesystem::path cursor = std::filesystem::current_path(ec);
  if (ec) {
    return std::nullopt;
  }

  for (int depth = 0; depth < 12; ++depth) {
    const auto fontPath = cursor / "assets" / "fonts" / "DefaultFont.ttf";
    if (std::filesystem::exists(fontPath, ec) && !ec) {
      return cursor;
    }

    if (!cursor.has_parent_path()) {
      break;
    }
    const std::filesystem::path parent = cursor.parent_path();
    if (parent == cursor) {
      break;
    }
    cursor = parent;
  }

  return std::nullopt;
}

bool writeManifest(const std::filesystem::path& path, bool useSdf, int sdfSpreadPx) {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }

  out << "{\n"
      << "  \"font\": \"./fonts/DefaultFont.otf\",\n"
      << "  \"ui-font-default\": \"./images/ui-font-default.png\"";

  if (useSdf) {
    out << ",\n"
        << "  \"font-render-mode\": \"sdf\",\n"
        << "  \"font-sdf-spread-px\": \"" << sdfSpreadPx << "\"\n";
  } else {
    out << "\n";
  }

  out << "}\n";
  return out.good();
}

std::vector<int> testCodepoints() {
  return {
      static_cast<int>(U' '),
      static_cast<int>(U'?'),
      static_cast<int>(U'A'),
      static_cast<int>(U'B'),
      static_cast<int>(U'V'),
      static_cast<int>(U'W'),
      static_cast<int>(U'g'),
      static_cast<int>(U'\u20AC'),
      static_cast<int>(U'\u03A9'),
      static_cast<int>(U'\u0416'),
  };
}

std::size_t countMidrangeAlphaSamples(const std::vector<std::uint8_t>& rgba) {
  std::size_t count = 0U;
  for (std::size_t i = 3U; i < rgba.size(); i += 4U) {
    const std::uint8_t alpha = rgba[i];
    if (alpha > 32U && alpha < 223U) {
      ++count;
    }
  }
  return count;
}

}  // namespace

int main() {
  bool ok = true;

  const auto workspaceRoot = findWorkspaceRoot();
  ok = expect(workspaceRoot.has_value(), "locate workspace root with assets/fonts/DefaultFont.ttf") && ok;
  if (!workspaceRoot.has_value()) {
    return 1;
  }

  const std::filesystem::path sourceFont = *workspaceRoot / "assets" / "fonts" / "DefaultFont.ttf";
  ok = expect(std::filesystem::exists(sourceFont), "source default ttf exists") && ok;

  const std::filesystem::path sandbox = std::filesystem::temp_directory_path() / "volt-font-codec-tests";
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
  std::filesystem::create_directories(sandbox / "assets" / "fonts", ec);
  std::filesystem::create_directories(sandbox / "assets" / "images", ec);

  const std::filesystem::path sandboxFont = sandbox / "assets" / "fonts" / "DefaultFont.otf";
  std::filesystem::copy_file(sourceFont, sandboxFont, std::filesystem::copy_options::overwrite_existing, ec);
  ok = expect(!ec, "copy ttf to .otf sandbox path") && ok;

  const std::filesystem::path sandboxManifest = sandbox / "assets" / "manifest.json";
  ok = expect(writeManifest(sandboxManifest, true, 12), "write sandbox manifest") && ok;

  const std::filesystem::path oldCwd = std::filesystem::current_path();
  std::filesystem::current_path(sandbox, ec);
  ok = expect(!ec, "switch to sandbox cwd") && ok;

  if (ok) {
    ok = expect(volt::io::ensureDefaultFontAtlas(), "build default font atlas from sfnt font") && ok;

    volt::io::FontMetrics metrics{};
    ok = expect(volt::io::defaultFontMetrics(metrics), "read default font metrics") && ok;
    ok = expect(metrics.bakePixelHeight > 1.0F, "bake pixel height should be positive") && ok;
    ok = expect(metrics.bakePixelHeight >= 60.0F, "sdf mode should increase bake pixel height") && ok;
    ok = expect(metrics.ascentPx > 0.0F, "ascent should be positive") && ok;
    ok = expect(metrics.descentPx < metrics.ascentPx, "descent should be below ascent") && ok;

    const int glyphA = volt::io::defaultFontGlyphIndexForCodepoint(U'A');
    const int glyphV = volt::io::defaultFontGlyphIndexForCodepoint(U'V');
    const int glyphSpace = volt::io::defaultFontGlyphIndexForCodepoint(U' ');
    const int glyphEuro = volt::io::defaultFontGlyphIndexForCodepoint(U'\u20AC');
    ok = expect(glyphA >= 0, "ASCII glyph lookup should succeed") && ok;
    ok = expect(glyphV >= 0, "second ASCII glyph lookup should succeed") && ok;
    ok = expect(glyphSpace >= 0, "space glyph lookup should succeed") && ok;
    ok = expect(glyphEuro >= 0, "extended glyph lookup should succeed or fallback") && ok;
    ok = expect(volt::io::defaultFontKerningAdvance(-1, glyphA) == 0.0F,
          "invalid kerning lookup should return zero") && ok;

    volt::io::FontVectorGlyph vectorGlyphA{};
    volt::io::FontVectorGlyph vectorGlyphSpace{};
    ok = expect(volt::io::defaultFontVectorGlyph(glyphA, vectorGlyphA), "vector glyph retrieval for A should succeed") && ok;
    ok = expect(volt::io::defaultFontVectorGlyph(glyphSpace, vectorGlyphSpace), "vector glyph retrieval for space should succeed") && ok;
    ok = expect(vectorGlyphA.curveCount > 0U, "outlined glyphs should keep their vector curve metadata") && ok;
    ok = expect(vectorGlyphSpace.curveCount == 0U, "space glyph should not borrow vector curves from later glyphs") && ok;

    float x = 0.0F;
    float y = 0.0F;
    volt::io::FontGlyphQuad quad{};
    ok = expect(volt::io::defaultFontPackedQuad(glyphA, x, y, quad), "packed quad retrieval should succeed") && ok;
    ok = expect(quad.s1 >= quad.s0 && quad.t1 >= quad.t0, "quad UVs should be ordered") && ok;
    ok = expect(x > 0.0F, "advance should increase pen x") && ok;

    const std::filesystem::path atlasPng = sandbox / "assets" / "images" / "ui-font-default.png";
    volt::io::FontGpuAtlas gpuAtlas{};
    const bool hasGpuAtlas = volt::io::defaultFontGpuAtlas(gpuAtlas) && !gpuAtlas.jobs.empty();
    ok = expect(std::filesystem::exists(atlasPng) || hasGpuAtlas,
          "font atlas should be available through GPU jobs or emitted image fallback") && ok;

    const std::vector<int> codepoints = testCodepoints();

    volt::io::detail::clearSfntFontCacheForTesting();

    volt::io::detail::SfntAtlasBuildOptions coverageOptions{};
    coverageOptions.rasterMode = volt::io::detail::SfntAtlasRasterMode::kCoverage;

    volt::io::detail::SfntAtlasResult coverageAtlas{};
    ok = expect(
             volt::io::detail::buildSfntAtlasFromFile(sandboxFont, 48.0F, codepoints, coverageOptions, coverageAtlas),
             "coverage atlas build should succeed") &&
         ok;
    ok = expect(coverageAtlas.success, "coverage atlas should report success") && ok;
    ok = expect(coverageAtlas.fontGlyphIndices.size() == coverageAtlas.glyphs.size(),
          "coverage atlas should preserve source glyph indices") && ok;
    ok = expect(volt::io::detail::sfntFontCacheEntryCountForTesting() == 1U,
                "cache should contain one parsed font after initial build") && ok;

    volt::io::detail::SfntAtlasBuildOptions sdfOptions{};
    sdfOptions.rasterMode = volt::io::detail::SfntAtlasRasterMode::kSignedDistanceField;
    sdfOptions.sdfSpreadPx = 12;

    volt::io::detail::SfntAtlasResult sdfAtlas{};
    ok = expect(volt::io::detail::buildSfntAtlasFromFile(sandboxFont, 48.0F, codepoints, sdfOptions, sdfAtlas),
                "sdf atlas build should succeed") && ok;
    ok = expect(sdfAtlas.success, "sdf atlas should report success") && ok;
    ok = expect(sdfAtlas.fontGlyphIndices.size() == sdfAtlas.glyphs.size(),
          "sdf atlas should preserve source glyph indices") && ok;
    ok = expect(volt::io::detail::sfntFontCacheEntryCountForTesting() == 1U,
                "second build from same file should reuse parsed cache entry") && ok;
    ok = expect(coverageAtlas.kerningPairsPx == sdfAtlas.kerningPairsPx,
          "kerning pairs should be identical across CPU atlas raster modes") && ok;

    const auto coverageGlyphA = coverageAtlas.codepointToGlyphIndex.find(U'A');
    const auto coverageGlyphV = coverageAtlas.codepointToGlyphIndex.find(U'V');
    ok = expect(coverageGlyphA != coverageAtlas.codepointToGlyphIndex.end(),
          "coverage atlas should map A to a glyph") && ok;
    ok = expect(coverageGlyphV != coverageAtlas.codepointToGlyphIndex.end(),
          "coverage atlas should map V to a glyph") && ok;
    if (coverageGlyphA != coverageAtlas.codepointToGlyphIndex.end() &&
      coverageGlyphV != coverageAtlas.codepointToGlyphIndex.end()) {
      const auto kerningIt = coverageAtlas.kerningPairsPx.find(
        makeKerningKey(static_cast<std::uint32_t>(coverageGlyphA->second),
               static_cast<std::uint32_t>(coverageGlyphV->second)));
      ok = expect(kerningIt == coverageAtlas.kerningPairsPx.end() || kerningIt->second <= 0.0F,
            "A/V kerning, if present, should tighten rather than expand spacing") && ok;
    }

    const std::size_t coverageMidrange = countMidrangeAlphaSamples(coverageAtlas.rgba);
    const std::size_t sdfMidrange = countMidrangeAlphaSamples(sdfAtlas.rgba);
    ok = expect(sdfMidrange > coverageMidrange,
                "sdf atlas should produce more midrange alpha samples than coverage") && ok;

    volt::io::detail::clearSfntFontCacheForTesting();
    const std::size_t cacheLimit = volt::io::detail::sfntFontCacheMaxEntriesForTesting();

    for (std::size_t i = 0U; i < cacheLimit + 4U && ok; ++i) {
      const std::filesystem::path uniqueFontPath =
          sandbox / "assets" / "fonts" / ("CacheFont-" + std::to_string(i) + ".ttf");

      ec.clear();
      std::filesystem::copy_file(sourceFont, uniqueFontPath, std::filesystem::copy_options::overwrite_existing, ec);
      ok = expect(!ec, "copy unique test font for cache eviction scenario") && ok;
      if (!ok) {
        break;
      }

      volt::io::detail::SfntAtlasResult uniqueAtlas{};
      ok = expect(
               volt::io::detail::buildSfntAtlasFromFile(uniqueFontPath, 36.0F, codepoints, coverageOptions, uniqueAtlas),
               "unique font atlas build should succeed") &&
           ok;
      ok = expect(uniqueAtlas.success, "unique font atlas should report success") && ok;
    }

    ok = expect(volt::io::detail::sfntFontCacheEntryCountForTesting() <= cacheLimit,
                "parsed font cache should remain bounded by configured maximum") && ok;
  }

  std::filesystem::current_path(oldCwd, ec);

  if (!ok) {
    std::cerr << "[font-codec-test] One or more tests failed." << '\n';
    return 1;
  }

  std::cout << "[font-codec-test] All font codec tests passed." << '\n';
  return 0;
}
