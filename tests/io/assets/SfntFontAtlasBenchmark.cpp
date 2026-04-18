#include "io/assets/SfntFontAtlas.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

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

struct BenchCase {
  const char* name;
  volt::io::detail::SfntAtlasRasterMode mode;
  float bakePixelHeight;
  int spreadPx;
};

bool runCase(const std::filesystem::path& fontPath,
             const std::vector<int>& codepoints,
             const BenchCase& benchCase,
             std::size_t warmIterations) {
  volt::io::detail::SfntAtlasBuildOptions options{};
  options.rasterMode = benchCase.mode;
  options.sdfSpreadPx = benchCase.spreadPx;

  volt::io::detail::clearSfntFontCacheForTesting();

  volt::io::detail::SfntAtlasResult coldResult{};
  const auto coldStart = std::chrono::steady_clock::now();
  const bool coldOk =
      volt::io::detail::buildSfntAtlasFromFile(fontPath, benchCase.bakePixelHeight, codepoints, options, coldResult);
  const auto coldStop = std::chrono::steady_clock::now();
  const double coldMs =
      std::chrono::duration<double, std::milli>(coldStop - coldStart).count();

  if (!coldOk || !coldResult.success) {
    std::cerr << "[font-atlas-bench] FAIL case=" << benchCase.name
              << " cold build failed: " << coldResult.error << '\n';
    return false;
  }

  double totalWarmMs = 0.0;
  std::uint32_t warmWidth = coldResult.atlasWidth;
  std::uint32_t warmHeight = coldResult.atlasHeight;

  for (std::size_t i = 0U; i < warmIterations; ++i) {
    volt::io::detail::SfntAtlasResult warmResult{};
    const auto warmStart = std::chrono::steady_clock::now();
    const bool warmOk =
        volt::io::detail::buildSfntAtlasFromFile(fontPath, benchCase.bakePixelHeight, codepoints, options, warmResult);
    const auto warmStop = std::chrono::steady_clock::now();

    const double warmMs =
        std::chrono::duration<double, std::milli>(warmStop - warmStart).count();
    totalWarmMs += warmMs;

    if (!warmOk || !warmResult.success) {
      std::cerr << "[font-atlas-bench] FAIL case=" << benchCase.name
                << " warm build failed: " << warmResult.error << '\n';
      return false;
    }

    warmWidth = warmResult.atlasWidth;
    warmHeight = warmResult.atlasHeight;
  }

  const double avgWarmMs = warmIterations == 0U ? 0.0 : (totalWarmMs / static_cast<double>(warmIterations));

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "[font-atlas-bench] case=" << benchCase.name
            << " glyphs=" << codepoints.size()
            << " atlas=" << warmWidth << "x" << warmHeight
            << " cold_ms=" << coldMs
            << " warm_avg_ms=" << avgWarmMs
            << " warm_iters=" << warmIterations
            << '\n';

  return true;
}

}  // namespace

int main() {
  const auto workspaceRoot = findWorkspaceRoot();
  if (!workspaceRoot.has_value()) {
    std::cerr << "[font-atlas-bench] FAIL: unable to locate workspace root" << '\n';
    return 1;
  }

  const std::filesystem::path fontPath = *workspaceRoot / "assets" / "fonts" / "DefaultFont.ttf";
  if (!std::filesystem::exists(fontPath)) {
    std::cerr << "[font-atlas-bench] FAIL: missing font at " << fontPath.string() << '\n';
    return 1;
  }

  const std::vector<int> codepoints = buildAtlasCodepointList();
  constexpr std::size_t kWarmIterations = 3U;

  const std::array<BenchCase, 3> benchCases = {
      BenchCase{"coverage-48", volt::io::detail::SfntAtlasRasterMode::kCoverage, 48.0F, 8},
      BenchCase{"sdf-96", volt::io::detail::SfntAtlasRasterMode::kSignedDistanceField, 96.0F, 12},
      BenchCase{"msdf-96", volt::io::detail::SfntAtlasRasterMode::kMultiChannelSignedDistanceField, 96.0F, 12},
  };

  bool ok = true;
  for (const BenchCase& benchCase : benchCases) {
    ok = runCase(fontPath, codepoints, benchCase, kWarmIterations) && ok;
  }

  return ok ? 0 : 1;
}
