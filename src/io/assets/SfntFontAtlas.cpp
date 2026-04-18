#include "SfntFontAtlas.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace volt::io::detail {
namespace {

constexpr std::uint32_t kMaxAtlasDimension = 4096U;
constexpr std::size_t kMaxFontFileBytes = 64U * 1024U * 1024U;
constexpr int kCurveSubdivisionMin = 4;
constexpr int kCurveSubdivisionMax = 24;
constexpr std::size_t kSfntFontCacheMaxEntries = 8U;
constexpr float kDistanceFieldInfinity = 1.0e20F;
constexpr float kDistanceFieldNegInfinity = -1.0e20F;

struct Point {
  float x{0.0F};
  float y{0.0F};
  bool onCurve{false};
};

using Contour = std::vector<Point>;
using Polyline = std::vector<std::array<float, 2>>;

struct GlyphOutline {
  int xMin{0};
  int yMin{0};
  int xMax{0};
  int yMax{0};
  std::vector<Contour> contours;
};

struct TableRecord {
  std::uint32_t offset{0};
  std::uint32_t length{0};
};

struct CmapGroup12 {
  std::uint32_t startCharCode{0};
  std::uint32_t endCharCode{0};
  std::uint32_t startGlyphId{0};
};

struct CmapLookup {
  enum class Kind {
    kNone,
    kFormat4,
    kFormat12,
  };

  Kind kind{Kind::kNone};
  std::uint32_t subtableOffset{0};
  std::uint32_t subtableLength{0};
  std::vector<CmapGroup12> groups;
};

struct SfntFont {
  std::vector<std::uint8_t> bytes;
  std::uint32_t sfntVersion{0};

  TableRecord head{};
  TableRecord hhea{};
  TableRecord maxp{};
  TableRecord hmtx{};
  TableRecord kern{};
  TableRecord cmap{};
  TableRecord loca{};
  TableRecord glyf{};

  std::uint16_t unitsPerEm{0};
  std::int16_t ascent{0};
  std::int16_t descent{0};
  std::int16_t lineGap{0};
  std::uint16_t numGlyphs{0};
  std::uint16_t numLongHorMetrics{0};
  std::int16_t indexToLocFormat{0};

  CmapLookup cmapLookup{};
};

struct SfntFontCacheEntry {
  std::filesystem::file_time_type lastWriteTime{};
  std::shared_ptr<SfntFont> font;
  std::uint64_t lastAccessTick{0U};
};

struct GlyphBitmap {
  SfntGlyphRaster metrics{};
  std::uint32_t widthPx{0};
  std::uint32_t heightPx{0};
  std::vector<std::uint8_t> alpha;
  std::vector<std::uint8_t> rgb;
};

struct GlyphRasterBounds {
  float leftUnits{0.0F};
  float topUnits{0.0F};
  std::uint32_t widthPx{0};
  std::uint32_t heightPx{0};
};

struct GlyphVectorJob {
  SfntGlyphRaster metrics{};
  std::uint16_t fontGlyphIndex{0U};
  std::uint32_t widthPx{0};
  std::uint32_t heightPx{0};
  std::uint32_t atlasX{0};
  std::uint32_t atlasY{0};
  std::vector<SfntGpuCurveSegment> curves;
};

struct GpuContourEdge {
  SfntGpuCurveSegment segment{};
  std::array<float, 2> startPoint{};
  std::array<float, 2> endPoint{};
  std::array<float, 2> startTangent{};
  std::array<float, 2> endTangent{};
};

static_assert(sizeof(SfntGpuCurveSegment) == 48U, "SfntGpuCurveSegment must match std430 shader layout");
static_assert(sizeof(SfntGpuGlyphJob) == 32U, "SfntGpuGlyphJob must match std430 shader layout");

struct MsdfEdgeSegment {
  std::array<float, 2> a{};
  std::array<float, 2> b{};
  std::uint8_t channelMask{0x07U};
};

struct MsdfPreparedSegment {
  float ax{0.0F};
  float ay{0.0F};
  float vx{0.0F};
  float vy{0.0F};
  float vv{0.0F};
  float xMin{0.0F};
  float xMax{0.0F};
  float yMin{0.0F};
  float yMax{0.0F};
  std::uint8_t channelMask{0x07U};
};

constexpr std::uint32_t makeTag(char a, char b, char c, char d) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8U) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

constexpr std::uint64_t makeAtlasKerningKey(std::uint32_t leftGlyphIndex, std::uint32_t rightGlyphIndex) {
  return (static_cast<std::uint64_t>(leftGlyphIndex) << 32U) |
         static_cast<std::uint64_t>(rightGlyphIndex);
}

constexpr std::uint32_t makeFontKerningKey(std::uint16_t leftGlyphIndex, std::uint16_t rightGlyphIndex) {
  return (static_cast<std::uint32_t>(leftGlyphIndex) << 16U) |
         static_cast<std::uint32_t>(rightGlyphIndex);
}

bool readU16Be(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t& outValue) {
  if (offset + 2U > bytes.size()) {
    return false;
  }
  outValue = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) |
                                        static_cast<std::uint16_t>(bytes[offset + 1U]));
  return true;
}

bool readI16Be(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::int16_t& outValue) {
  std::uint16_t raw = 0U;
  if (!readU16Be(bytes, offset, raw)) {
    return false;
  }
  outValue = static_cast<std::int16_t>(raw);
  return true;
}

bool readU32Be(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t& outValue) {
  if (offset + 4U > bytes.size()) {
    return false;
  }
  outValue = (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
             (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
             (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
             static_cast<std::uint32_t>(bytes[offset + 3U]);
  return true;
}

bool readKerningPairs(const SfntFont& font,
                      std::unordered_map<std::uint32_t, std::int16_t>& outKerningUnits,
                      std::string& outError) {
  outKerningUnits.clear();
  if (font.kern.length == 0U) {
    return true;
  }

  const std::size_t tableStart = static_cast<std::size_t>(font.kern.offset);
  const std::size_t tableEnd = tableStart + static_cast<std::size_t>(font.kern.length);
  if (tableEnd > font.bytes.size() || font.kern.length < 4U) {
    outError = "invalid kern table";
    return false;
  }

  std::uint16_t version = 0U;
  std::uint16_t subtableCount = 0U;
  if (!readU16Be(font.bytes, tableStart + 0U, version) ||
      !readU16Be(font.bytes, tableStart + 2U, subtableCount)) {
    outError = "invalid kern table header";
    return false;
  }

  if (version != 0U) {
    return true;
  }

  std::size_t cursor = tableStart + 4U;
  for (std::uint16_t tableIndex = 0U; tableIndex < subtableCount; ++tableIndex) {
    if (cursor + 6U > tableEnd) {
      outError = "invalid kern subtable header";
      return false;
    }

    std::uint16_t subtableVersion = 0U;
    std::uint16_t subtableLength = 0U;
    std::uint16_t coverage = 0U;
    if (!readU16Be(font.bytes, cursor + 0U, subtableVersion) ||
        !readU16Be(font.bytes, cursor + 2U, subtableLength) ||
        !readU16Be(font.bytes, cursor + 4U, coverage)) {
      outError = "invalid kern subtable";
      return false;
    }

    if (subtableLength < 6U || cursor + static_cast<std::size_t>(subtableLength) > tableEnd) {
      outError = "invalid kern subtable length";
      return false;
    }

    const std::uint8_t format = static_cast<std::uint8_t>(coverage >> 8U);
    const std::uint8_t flags = static_cast<std::uint8_t>(coverage & 0x00FFU);
    const bool horizontal = (flags & 0x01U) != 0U;
    const bool minimumValues = (flags & 0x02U) != 0U;
    const bool crossStream = (flags & 0x04U) != 0U;
    const bool overrideValues = (flags & 0x08U) != 0U;

    if (subtableVersion == 0U && format == 0U && horizontal && !minimumValues && !crossStream) {
      if (subtableLength < 14U) {
        outError = "invalid kern format 0 subtable";
        return false;
      }

      std::uint16_t pairCount = 0U;
      if (!readU16Be(font.bytes, cursor + 6U, pairCount)) {
        outError = "invalid kern pair count";
        return false;
      }

      std::size_t pairCursor = cursor + 14U;
      const std::size_t pairBytes = static_cast<std::size_t>(pairCount) * 6U;
      if (pairCursor + pairBytes > cursor + static_cast<std::size_t>(subtableLength)) {
        outError = "invalid kern pair data";
        return false;
      }

      for (std::uint16_t pairIndex = 0U; pairIndex < pairCount; ++pairIndex) {
        std::uint16_t left = 0U;
        std::uint16_t right = 0U;
        std::int16_t value = 0;
        if (!readU16Be(font.bytes, pairCursor + 0U, left) ||
            !readU16Be(font.bytes, pairCursor + 2U, right) ||
            !readI16Be(font.bytes, pairCursor + 4U, value)) {
          outError = "invalid kern pair record";
          return false;
        }

        const std::uint32_t key = makeFontKerningKey(left, right);
        if (overrideValues) {
          outKerningUnits[key] = value;
        } else {
          outKerningUnits[key] = static_cast<std::int16_t>(outKerningUnits[key] + value);
        }
        pairCursor += 6U;
      }
    }

    cursor += static_cast<std::size_t>(subtableLength);
  }

  return true;
}

bool buildAtlasKerningPairs(const SfntFont& font,
                            const std::vector<std::uint16_t>& fontGlyphIndices,
                            float scale,
                            std::unordered_map<std::uint64_t, float>& outKerningPairsPx,
                            std::string& outError) {
  outKerningPairsPx.clear();
  if (fontGlyphIndices.empty()) {
    return true;
  }

  std::unordered_map<std::uint32_t, std::int16_t> kerningUnits;
  if (!readKerningPairs(font, kerningUnits, outError)) {
    return false;
  }

  if (kerningUnits.empty()) {
    return true;
  }

  for (std::size_t leftIndex = 0U; leftIndex < fontGlyphIndices.size(); ++leftIndex) {
    for (std::size_t rightIndex = 0U; rightIndex < fontGlyphIndices.size(); ++rightIndex) {
      const auto it = kerningUnits.find(makeFontKerningKey(fontGlyphIndices[leftIndex], fontGlyphIndices[rightIndex]));
      if (it == kerningUnits.end() || it->second == 0) {
        continue;
      }

      outKerningPairsPx.emplace(
          makeAtlasKerningKey(static_cast<std::uint32_t>(leftIndex), static_cast<std::uint32_t>(rightIndex)),
          static_cast<float>(it->second) * scale);
    }
  }

  return true;
}

bool readS8(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::int8_t& outValue) {
  if (offset + 1U > bytes.size()) {
    return false;
  }
  outValue = static_cast<std::int8_t>(bytes[offset]);
  return true;
}

bool readBinaryFile(const std::filesystem::path& path, std::vector<std::uint8_t>& outBytes) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }

  in.seekg(0, std::ios::end);
  const std::streamoff length = in.tellg();
  if (length <= 0 || static_cast<std::uint64_t>(length) > kMaxFontFileBytes) {
    return false;
  }
  in.seekg(0, std::ios::beg);

  outBytes.resize(static_cast<std::size_t>(length));
  in.read(reinterpret_cast<char*>(outBytes.data()), length);
  return in.good() || in.eof();
}

std::unordered_map<std::string, SfntFontCacheEntry>& sfntFontCache() {
  static std::unordered_map<std::string, SfntFontCacheEntry> cache;
  return cache;
}

std::uint64_t& sfntFontCacheAccessCounter() {
  static std::uint64_t counter = 0U;
  return counter;
}

void trimSfntFontCacheIfNeeded() {
  auto& cache = sfntFontCache();
  while (cache.size() > kSfntFontCacheMaxEntries) {
    auto victim = cache.end();
    std::uint64_t oldestTick = std::numeric_limits<std::uint64_t>::max();

    for (auto it = cache.begin(); it != cache.end(); ++it) {
      if (!it->second.font || it->second.lastAccessTick < oldestTick) {
        oldestTick = it->second.lastAccessTick;
        victim = it;
      }
    }

    if (victim == cache.end()) {
      break;
    }
    cache.erase(victim);
  }
}

std::string normalizedPathKey(const std::filesystem::path& path) {
  std::error_code ec;
  const std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
  if (ec) {
    return path.lexically_normal().string();
  }
  return absolutePath.lexically_normal().string();
}

bool parseTableDirectory(const std::vector<std::uint8_t>& bytes,
                         std::unordered_map<std::uint32_t, TableRecord>& outTables,
                         std::uint32_t& outSfntVersion) {
  if (bytes.size() < 12U) {
    return false;
  }

  std::uint32_t sfntVersion = 0U;
  std::uint16_t numTables = 0U;
  if (!readU32Be(bytes, 0U, sfntVersion) || !readU16Be(bytes, 4U, numTables)) {
    return false;
  }

  const std::size_t directoryBytes = 12U + static_cast<std::size_t>(numTables) * 16U;
  if (directoryBytes > bytes.size()) {
    return false;
  }

  outTables.clear();
  outTables.reserve(numTables);

  for (std::size_t i = 0U; i < numTables; ++i) {
    const std::size_t recordOffset = 12U + i * 16U;
    std::uint32_t tag = 0U;
    std::uint32_t tableOffset = 0U;
    std::uint32_t tableLength = 0U;
    if (!readU32Be(bytes, recordOffset + 0U, tag) ||
        !readU32Be(bytes, recordOffset + 8U, tableOffset) ||
        !readU32Be(bytes, recordOffset + 12U, tableLength)) {
      return false;
    }

    if (tableOffset > bytes.size()) {
      return false;
    }
    if (tableLength > bytes.size() - tableOffset) {
      return false;
    }

    outTables[tag] = TableRecord{tableOffset, tableLength};
  }

  outSfntVersion = sfntVersion;
  return true;
}

bool parseHeadTable(SfntFont& outFont) {
  if (outFont.head.length < 54U) {
    return false;
  }

  std::uint16_t unitsPerEm = 0U;
  std::int16_t indexToLocFormat = 0;
  if (!readU16Be(outFont.bytes, static_cast<std::size_t>(outFont.head.offset) + 18U, unitsPerEm) ||
      !readI16Be(outFont.bytes, static_cast<std::size_t>(outFont.head.offset) + 50U, indexToLocFormat)) {
    return false;
  }

  if (unitsPerEm == 0U) {
    return false;
  }

  outFont.unitsPerEm = unitsPerEm;
  outFont.indexToLocFormat = indexToLocFormat;
  return indexToLocFormat == 0 || indexToLocFormat == 1;
}

bool parseHheaTable(SfntFont& outFont) {
  if (outFont.hhea.length < 36U) {
    return false;
  }

  std::int16_t ascent = 0;
  std::int16_t descent = 0;
  std::int16_t lineGap = 0;
  std::uint16_t numLongHorMetrics = 0U;
  if (!readI16Be(outFont.bytes, static_cast<std::size_t>(outFont.hhea.offset) + 4U, ascent) ||
      !readI16Be(outFont.bytes, static_cast<std::size_t>(outFont.hhea.offset) + 6U, descent) ||
      !readI16Be(outFont.bytes, static_cast<std::size_t>(outFont.hhea.offset) + 8U, lineGap) ||
      !readU16Be(outFont.bytes, static_cast<std::size_t>(outFont.hhea.offset) + 34U, numLongHorMetrics)) {
    return false;
  }

  outFont.ascent = ascent;
  outFont.descent = descent;
  outFont.lineGap = lineGap;
  outFont.numLongHorMetrics = numLongHorMetrics;
  return true;
}

bool parseMaxpTable(SfntFont& outFont) {
  if (outFont.maxp.length < 6U) {
    return false;
  }

  std::uint16_t numGlyphs = 0U;
  if (!readU16Be(outFont.bytes, static_cast<std::size_t>(outFont.maxp.offset) + 4U, numGlyphs)) {
    return false;
  }

  outFont.numGlyphs = numGlyphs;
  if (numGlyphs == 0U) {
    return false;
  }

  return true;
}

bool validateHorizontalMetricsTable(const SfntFont& font) {
  if (font.numLongHorMetrics == 0U || font.numLongHorMetrics > font.numGlyphs) {
    return false;
  }

  const std::size_t longMetricsBytes = static_cast<std::size_t>(font.numLongHorMetrics) * 4U;
  const std::size_t lsbCount = static_cast<std::size_t>(font.numGlyphs - font.numLongHorMetrics);
  const std::size_t lsbBytes = lsbCount * 2U;

  return font.hmtx.length >= longMetricsBytes + lsbBytes;
}

bool validateLocaTable(const SfntFont& font) {
  const std::size_t requiredEntries = static_cast<std::size_t>(font.numGlyphs) + 1U;
  const std::size_t entryBytes = font.indexToLocFormat == 0 ? 2U : 4U;
  return font.loca.length >= requiredEntries * entryBytes;
}

int scoreCmapSubtable(std::uint16_t platformId, std::uint16_t encodingId, std::uint16_t format) {
  if (format == 12U) {
    if (platformId == 3U && encodingId == 10U) {
      return 120;
    }
    if (platformId == 0U) {
      return 110;
    }
    if (platformId == 3U && encodingId == 1U) {
      return 100;
    }
    return 60;
  }

  if (format == 4U) {
    if (platformId == 3U && encodingId == 1U) {
      return 90;
    }
    if (platformId == 0U) {
      return 80;
    }
    if (platformId == 3U && encodingId == 10U) {
      return 70;
    }
    return 40;
  }

  return -1;
}

bool parseCmap(const std::vector<std::uint8_t>& bytes,
               const TableRecord& cmapTable,
               CmapLookup& outLookup) {
  if (cmapTable.length < 4U) {
    return false;
  }

  std::uint16_t numTables = 0U;
  if (!readU16Be(bytes, static_cast<std::size_t>(cmapTable.offset) + 2U, numTables)) {
    return false;
  }

  const std::size_t recordsOffset = static_cast<std::size_t>(cmapTable.offset) + 4U;
  const std::size_t recordsSize = static_cast<std::size_t>(numTables) * 8U;
  if (recordsOffset + recordsSize > bytes.size()) {
    return false;
  }

  int bestScore = -1;
  CmapLookup best{};

  for (std::size_t i = 0U; i < numTables; ++i) {
    const std::size_t recordOffset = recordsOffset + i * 8U;
    std::uint16_t platformId = 0U;
    std::uint16_t encodingId = 0U;
    std::uint32_t subtableRelativeOffset = 0U;

    if (!readU16Be(bytes, recordOffset + 0U, platformId) ||
        !readU16Be(bytes, recordOffset + 2U, encodingId) ||
        !readU32Be(bytes, recordOffset + 4U, subtableRelativeOffset)) {
      continue;
    }

    if (subtableRelativeOffset >= cmapTable.length) {
      continue;
    }

    const std::size_t subtableOffset = static_cast<std::size_t>(cmapTable.offset) +
                                       static_cast<std::size_t>(subtableRelativeOffset);
    if (subtableOffset + 2U > bytes.size()) {
      continue;
    }

    std::uint16_t format = 0U;
    if (!readU16Be(bytes, subtableOffset, format)) {
      continue;
    }

    std::uint32_t subtableLength = 0U;
    if (format == 4U) {
      std::uint16_t len16 = 0U;
      if (!readU16Be(bytes, subtableOffset + 2U, len16)) {
        continue;
      }
      subtableLength = len16;
    } else if (format == 12U) {
      if (!readU32Be(bytes, subtableOffset + 4U, subtableLength)) {
        continue;
      }
    } else {
      continue;
    }

    if (subtableLength == 0U) {
      continue;
    }
    if (subtableOffset + static_cast<std::size_t>(subtableLength) > bytes.size()) {
      continue;
    }

    const int score = scoreCmapSubtable(platformId, encodingId, format);
    if (score <= bestScore) {
      continue;
    }

    CmapLookup candidate{};
    candidate.kind = format == 12U ? CmapLookup::Kind::kFormat12 : CmapLookup::Kind::kFormat4;
    candidate.subtableOffset = static_cast<std::uint32_t>(subtableOffset);
    candidate.subtableLength = subtableLength;

    if (candidate.kind == CmapLookup::Kind::kFormat12) {
      std::uint32_t numGroups = 0U;
      if (!readU32Be(bytes, subtableOffset + 12U, numGroups)) {
        continue;
      }

      const std::size_t groupsOffset = subtableOffset + 16U;
      const std::size_t groupsBytes = static_cast<std::size_t>(numGroups) * 12U;
      if (groupsOffset + groupsBytes > subtableOffset + static_cast<std::size_t>(subtableLength)) {
        continue;
      }

      candidate.groups.reserve(numGroups);
      for (std::size_t g = 0U; g < numGroups; ++g) {
        const std::size_t go = groupsOffset + g * 12U;
        std::uint32_t startCharCode = 0U;
        std::uint32_t endCharCode = 0U;
        std::uint32_t startGlyphId = 0U;
        if (!readU32Be(bytes, go + 0U, startCharCode) ||
            !readU32Be(bytes, go + 4U, endCharCode) ||
            !readU32Be(bytes, go + 8U, startGlyphId)) {
          candidate.groups.clear();
          break;
        }
        if (startCharCode > endCharCode) {
          candidate.groups.clear();
          break;
        }
        candidate.groups.push_back(CmapGroup12{startCharCode, endCharCode, startGlyphId});
      }

      if (candidate.groups.empty()) {
        continue;
      }
    }

    best = std::move(candidate);
    bestScore = score;
  }

  if (best.kind == CmapLookup::Kind::kNone) {
    return false;
  }

  outLookup = std::move(best);
  return true;
}

bool lookupCmapFormat12(const CmapLookup& cmap, std::uint32_t codepoint, std::uint16_t& outGlyphIndex) {
  std::size_t left = 0U;
  std::size_t right = cmap.groups.size();

  while (left < right) {
    const std::size_t mid = left + (right - left) / 2U;
    const CmapGroup12& group = cmap.groups[mid];

    if (codepoint < group.startCharCode) {
      right = mid;
      continue;
    }
    if (codepoint > group.endCharCode) {
      left = mid + 1U;
      continue;
    }

    const std::uint64_t glyph = static_cast<std::uint64_t>(group.startGlyphId) +
                                static_cast<std::uint64_t>(codepoint - group.startCharCode);
    if (glyph > std::numeric_limits<std::uint16_t>::max()) {
      return false;
    }

    outGlyphIndex = static_cast<std::uint16_t>(glyph);
    return true;
  }

  outGlyphIndex = 0U;
  return true;
}

bool lookupCmapFormat4(const SfntFont& font, std::uint32_t codepoint, std::uint16_t& outGlyphIndex) {
  if (codepoint > 0xFFFFU) {
    outGlyphIndex = 0U;
    return true;
  }

  const std::size_t base = static_cast<std::size_t>(font.cmapLookup.subtableOffset);
  const std::size_t end = base + static_cast<std::size_t>(font.cmapLookup.subtableLength);
  if (end > font.bytes.size() || end < base + 16U) {
    return false;
  }

  std::uint16_t segCountX2 = 0U;
  if (!readU16Be(font.bytes, base + 6U, segCountX2) || (segCountX2 & 1U) != 0U) {
    return false;
  }

  const std::size_t segCount = static_cast<std::size_t>(segCountX2 / 2U);
  const std::size_t endCodeOffset = base + 14U;
  const std::size_t reservedPadOffset = endCodeOffset + segCount * 2U;
  const std::size_t startCodeOffset = reservedPadOffset + 2U;
  const std::size_t idDeltaOffset = startCodeOffset + segCount * 2U;
  const std::size_t idRangeOffsetOffset = idDeltaOffset + segCount * 2U;

  if (idRangeOffsetOffset + segCount * 2U > end) {
    return false;
  }

  const std::uint16_t cp16 = static_cast<std::uint16_t>(codepoint);

  for (std::size_t i = 0U; i < segCount; ++i) {
    std::uint16_t endCode = 0U;
    std::uint16_t startCode = 0U;
    std::int16_t idDelta = 0;
    std::uint16_t idRangeOffset = 0U;

    if (!readU16Be(font.bytes, endCodeOffset + i * 2U, endCode) ||
        !readU16Be(font.bytes, startCodeOffset + i * 2U, startCode) ||
        !readI16Be(font.bytes, idDeltaOffset + i * 2U, idDelta) ||
        !readU16Be(font.bytes, idRangeOffsetOffset + i * 2U, idRangeOffset)) {
      return false;
    }

    if (cp16 > endCode) {
      continue;
    }

    if (cp16 < startCode) {
      outGlyphIndex = 0U;
      return true;
    }

    if (idRangeOffset == 0U) {
      outGlyphIndex = static_cast<std::uint16_t>((static_cast<std::int32_t>(cp16) + idDelta) & 0xFFFF);
      return true;
    }

    const std::size_t idRangeWordAddress = idRangeOffsetOffset + i * 2U;
    const std::size_t glyphWordAddress = idRangeWordAddress +
                                         static_cast<std::size_t>(idRangeOffset) +
                                         static_cast<std::size_t>(cp16 - startCode) * 2U;

    if (glyphWordAddress + 2U > end) {
      return false;
    }

    std::uint16_t glyphId = 0U;
    if (!readU16Be(font.bytes, glyphWordAddress, glyphId)) {
      return false;
    }

    if (glyphId == 0U) {
      outGlyphIndex = 0U;
      return true;
    }

    outGlyphIndex = static_cast<std::uint16_t>((static_cast<std::int32_t>(glyphId) + idDelta) & 0xFFFF);
    return true;
  }

  outGlyphIndex = 0U;
  return true;
}

bool glyphIndexForCodepoint(const SfntFont& font, std::uint32_t codepoint, std::uint16_t& outGlyphIndex) {
  if (font.cmapLookup.kind == CmapLookup::Kind::kFormat12) {
    return lookupCmapFormat12(font.cmapLookup, codepoint, outGlyphIndex);
  }

  if (font.cmapLookup.kind == CmapLookup::Kind::kFormat4) {
    return lookupCmapFormat4(font, codepoint, outGlyphIndex);
  }

  return false;
}

bool readGlyphOffset(const SfntFont& font, std::uint16_t glyphIndex, std::uint32_t& outOffset) {
  const std::size_t base = static_cast<std::size_t>(font.loca.offset);
  if (font.indexToLocFormat == 0) {
    std::uint16_t raw = 0U;
    if (!readU16Be(font.bytes, base + static_cast<std::size_t>(glyphIndex) * 2U, raw)) {
      return false;
    }
    outOffset = static_cast<std::uint32_t>(raw) * 2U;
    return true;
  }

  return readU32Be(font.bytes, base + static_cast<std::size_t>(glyphIndex) * 4U, outOffset);
}

bool glyphDataRange(const SfntFont& font,
                    std::uint16_t glyphIndex,
                    std::size_t& outStart,
                    std::size_t& outEnd) {
  if (glyphIndex >= font.numGlyphs) {
    return false;
  }

  std::uint32_t startOffset = 0U;
  std::uint32_t endOffset = 0U;
  if (!readGlyphOffset(font, glyphIndex, startOffset) ||
      !readGlyphOffset(font, static_cast<std::uint16_t>(glyphIndex + 1U), endOffset)) {
    return false;
  }

  if (endOffset < startOffset || endOffset > font.glyf.length) {
    return false;
  }

  outStart = static_cast<std::size_t>(font.glyf.offset) + static_cast<std::size_t>(startOffset);
  outEnd = static_cast<std::size_t>(font.glyf.offset) + static_cast<std::size_t>(endOffset);
  return outEnd <= font.bytes.size();
}

bool glyphHorizontalMetrics(const SfntFont& font,
                            std::uint16_t glyphIndex,
                            std::uint16_t& outAdvanceWidth,
                            std::int16_t& outLeftSideBearing) {
  if (glyphIndex >= font.numGlyphs) {
    return false;
  }

  const std::size_t hmtxBase = static_cast<std::size_t>(font.hmtx.offset);

  if (glyphIndex < font.numLongHorMetrics) {
    const std::size_t offset = hmtxBase + static_cast<std::size_t>(glyphIndex) * 4U;
    return readU16Be(font.bytes, offset + 0U, outAdvanceWidth) &&
           readI16Be(font.bytes, offset + 2U, outLeftSideBearing);
  }

  const std::size_t lastLongOffset = hmtxBase + static_cast<std::size_t>(font.numLongHorMetrics - 1U) * 4U;
  if (!readU16Be(font.bytes, lastLongOffset, outAdvanceWidth)) {
    return false;
  }

  const std::size_t lsbOffset = hmtxBase +
                                static_cast<std::size_t>(font.numLongHorMetrics) * 4U +
                                static_cast<std::size_t>(glyphIndex - font.numLongHorMetrics) * 2U;
  return readI16Be(font.bytes, lsbOffset, outLeftSideBearing);
}

bool appendQuadraticToPolyline(const std::array<float, 2>& p0,
                               const std::array<float, 2>& p1,
                               const std::array<float, 2>& p2,
                               float scale,
                               Polyline& polyline) {
  const float dx1 = p1[0] - p0[0];
  const float dy1 = p1[1] - p0[1];
  const float dx2 = p2[0] - p1[0];
  const float dy2 = p2[1] - p1[1];
  const float chordX = p2[0] - p0[0];
  const float chordY = p2[1] - p0[1];

  const float controlDistance = std::sqrt(dx1 * dx1 + dy1 * dy1) +
                                std::sqrt(dx2 * dx2 + dy2 * dy2);
  const float chordDistance = std::sqrt(chordX * chordX + chordY * chordY);
  const float flatness = std::max(0.0F, controlDistance - chordDistance) * std::max(scale, 0.01F);

  int steps = static_cast<int>(std::ceil(flatness * 0.25F)) + kCurveSubdivisionMin;
  steps = std::clamp(steps, kCurveSubdivisionMin, kCurveSubdivisionMax);

  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const float invT = 1.0F - t;

    const float x = invT * invT * p0[0] + 2.0F * invT * t * p1[0] + t * t * p2[0];
    const float y = invT * invT * p0[1] + 2.0F * invT * t * p1[1] + t * t * p2[1];
    polyline.push_back({x, y});
  }

  return true;
}

bool buildFlattenedPolylines(const GlyphOutline& outline,
                             float scale,
                             std::vector<Polyline>& outPolylines) {
  outPolylines.clear();
  outPolylines.reserve(outline.contours.size());

  for (const Contour& contour : outline.contours) {
    if (contour.empty()) {
      continue;
    }

    const std::size_t count = contour.size();
    auto pointAt = [&](std::size_t index) -> const Point& {
      return contour[index % count];
    };

    std::array<float, 2> start{};
    std::size_t index = 0U;

    if (contour[0].onCurve) {
      start = {contour[0].x, contour[0].y};
      index = 1U;
    } else if (contour[count - 1U].onCurve) {
      start = {contour[count - 1U].x, contour[count - 1U].y};
      index = 0U;
    } else {
      start = {(contour[count - 1U].x + contour[0].x) * 0.5F,
               (contour[count - 1U].y + contour[0].y) * 0.5F};
      index = 0U;
    }

    Polyline polyline;
    polyline.reserve(count * 2U);
    polyline.push_back(start);

    std::array<float, 2> current = start;
    std::size_t consumed = 0U;

    while (consumed < count) {
      const Point& p1 = pointAt(index);
      if (p1.onCurve) {
        const std::array<float, 2> next = {p1.x, p1.y};
        if (next != current) {
          polyline.push_back(next);
          current = next;
        }
        index = (index + 1U) % count;
        ++consumed;
        continue;
      }

      const Point& p2 = pointAt(index + 1U);
      const std::array<float, 2> control = {p1.x, p1.y};
      if (p2.onCurve) {
        const std::array<float, 2> end = {p2.x, p2.y};
        appendQuadraticToPolyline(current, control, end, scale, polyline);
        current = end;
        index = (index + 2U) % count;
        consumed += 2U;
      } else {
        const std::array<float, 2> end = {(p1.x + p2.x) * 0.5F, (p1.y + p2.y) * 0.5F};
        appendQuadraticToPolyline(current, control, end, scale, polyline);
        current = end;
        index = (index + 1U) % count;
        ++consumed;
      }
    }

    if (polyline.size() < 2U) {
      continue;
    }

    if (polyline.front() != polyline.back()) {
      polyline.push_back(polyline.front());
    }

    outPolylines.push_back(std::move(polyline));
  }

  return !outPolylines.empty();
}

bool pointInsideEvenOdd(float x, float y, const std::vector<Polyline>& polylines) {
  bool inside = false;

  for (const Polyline& polyline : polylines) {
    if (polyline.size() < 2U) {
      continue;
    }

    for (std::size_t i = 0U; i + 1U < polyline.size(); ++i) {
      const auto& a = polyline[i];
      const auto& b = polyline[i + 1U];

      const bool yCheck = ((a[1] > y) != (b[1] > y));
      if (!yCheck) {
        continue;
      }

      const float dy = b[1] - a[1];
      if (std::abs(dy) < 1e-6F) {
        continue;
      }

      const float xAtY = a[0] + ((y - a[1]) * (b[0] - a[0]) / dy);
      if (xAtY > x) {
        inside = !inside;
      }
    }
  }

  return inside;
}

bool buildMsdfEdgeSegments(const std::vector<Polyline>& polylines,
                           std::vector<MsdfEdgeSegment>& outSegments) {
  outSegments.clear();

  for (const Polyline& polyline : polylines) {
    if (polyline.size() < 2U) {
      continue;
    }

    const std::size_t segmentCount = polyline.size() - 1U;
    for (std::size_t i = 0U; i < segmentCount; ++i) {
      const auto& a = polyline[i];
      const auto& b = polyline[i + 1U];
      if (a == b) {
        continue;
      }

      MsdfEdgeSegment segment{};
      segment.a = a;
      segment.b = b;
      segment.channelMask = segmentCount < 3U ? 0x07U : static_cast<std::uint8_t>(1U << (i % 3U));
      outSegments.push_back(segment);
    }
  }

  return !outSegments.empty();
}

float pointToAabbDistanceSquared(float px,
                                 float py,
                                 float xMin,
                                 float xMax,
                                 float yMin,
                                 float yMax) {
  float dx = 0.0F;
  if (px < xMin) {
    dx = xMin - px;
  } else if (px > xMax) {
    dx = px - xMax;
  }

  float dy = 0.0F;
  if (py < yMin) {
    dy = yMin - py;
  } else if (py > yMax) {
    dy = py - yMax;
  }

  return (dx * dx) + (dy * dy);
}

float pointToPreparedSegmentDistanceSquared(float px,
                                            float py,
                                            const MsdfPreparedSegment& segment) {
  const float wx = px - segment.ax;
  const float wy = py - segment.ay;

  if (segment.vv <= 1e-12F) {
    return (wx * wx) + (wy * wy);
  }

  const float t = std::clamp(((wx * segment.vx) + (wy * segment.vy)) / segment.vv, 0.0F, 1.0F);
  const float sx = segment.ax + (segment.vx * t);
  const float sy = segment.ay + (segment.vy * t);
  const float dx = px - sx;
  const float dy = py - sy;
  return (dx * dx) + (dy * dy);
}

void buildPreparedMsdfSegments(const std::vector<MsdfEdgeSegment>& segments,
                               std::vector<MsdfPreparedSegment>& outPrepared) {
  outPrepared.clear();
  outPrepared.reserve(segments.size());

  for (const MsdfEdgeSegment& segment : segments) {
    MsdfPreparedSegment prepared{};
    prepared.ax = segment.a[0];
    prepared.ay = segment.a[1];
    prepared.vx = segment.b[0] - segment.a[0];
    prepared.vy = segment.b[1] - segment.a[1];
    prepared.vv = (prepared.vx * prepared.vx) + (prepared.vy * prepared.vy);
    prepared.xMin = std::min(segment.a[0], segment.b[0]);
    prepared.xMax = std::max(segment.a[0], segment.b[0]);
    prepared.yMin = std::min(segment.a[1], segment.b[1]);
    prepared.yMax = std::max(segment.a[1], segment.b[1]);
    prepared.channelMask = segment.channelMask;
    outPrepared.push_back(prepared);
  }
}

std::uint8_t encodeSignedDistanceToByte(float signedDistance, float spreadPx) {
  const float normalized = std::clamp(
      0.5F + (signedDistance / std::max(spreadPx, 1.0F)) * 0.5F,
      0.0F,
      1.0F);
  return static_cast<std::uint8_t>(std::lround(normalized * 255.0F));
}

bool computeGlyphRasterBounds(const GlyphOutline& outline,
                              float scale,
                              const SfntAtlasBuildOptions& buildOptions,
                              GlyphRasterBounds& outBounds) {
  outBounds = GlyphRasterBounds{};

  if (outline.contours.empty() || outline.xMin >= outline.xMax || outline.yMin >= outline.yMax) {
    return true;
  }

  const bool useDistanceField =
      buildOptions.rasterMode == SfntAtlasRasterMode::kSignedDistanceField ||
      buildOptions.rasterMode == SfntAtlasRasterMode::kMultiChannelSignedDistanceField;

  constexpr int kCoveragePaddingPx = 1;
  const int distanceFieldSpreadPx = std::clamp(buildOptions.sdfSpreadPx, 2, 64);
  const int paddingPx = useDistanceField
                            ? std::clamp(distanceFieldSpreadPx + 2, 4, 16)
                            : kCoveragePaddingPx;

  const float clampedScale = std::max(scale, 0.01F);
  const float inverseScale = 1.0F / clampedScale;

  outBounds.leftUnits = static_cast<float>(outline.xMin) -
                        static_cast<float>(paddingPx) * inverseScale;
  outBounds.topUnits = static_cast<float>(outline.yMax) +
                       static_cast<float>(paddingPx) * inverseScale;

  const float widthPxFloat = (static_cast<float>(outline.xMax - outline.xMin) * scale) +
                             static_cast<float>(paddingPx * 2);
  const float heightPxFloat = (static_cast<float>(outline.yMax - outline.yMin) * scale) +
                              static_cast<float>(paddingPx * 2);

  outBounds.widthPx = std::max(1U, static_cast<std::uint32_t>(std::ceil(widthPxFloat)));
  outBounds.heightPx = std::max(1U, static_cast<std::uint32_t>(std::ceil(heightPxFloat)));
  return outBounds.widthPx <= 2048U && outBounds.heightPx <= 2048U;
}

std::array<float, 2> normalizeGlyphPoint(float xUnits,
                                         float yUnits,
                                         const GlyphRasterBounds& bounds,
                                         float scale) {
  const float safeWidth = static_cast<float>(std::max(bounds.widthPx, 1U));
  const float safeHeight = static_cast<float>(std::max(bounds.heightPx, 1U));
  const float pixelX = (xUnits - bounds.leftUnits) * scale;
  const float pixelY = (bounds.topUnits - yUnits) * scale;
  return {
      pixelX / safeWidth,
      pixelY / safeHeight,
  };
}

SfntGpuCurveSegment makeQuadraticSegment(const std::array<float, 2>& p0,
                                         const std::array<float, 2>& p1,
                                         const std::array<float, 2>& p2) {
  SfntGpuCurveSegment segment{};
  segment.p0x = p0[0];
  segment.p0y = p0[1];
  segment.p1x = p1[0];
  segment.p1y = p1[1];
  segment.p2x = p2[0];
  segment.p2y = p2[1];
  segment.type = 0;
  segment.channelMask = 0x07;
  segment.contourSign = 1;
  return segment;
}

SfntGpuCurveSegment makeLinearCubicSegment(const std::array<float, 2>& p0,
                                           const std::array<float, 2>& p1) {
  const std::array<float, 2> c1 = {
      p0[0] + ((p1[0] - p0[0]) / 3.0F),
      p0[1] + ((p1[1] - p0[1]) / 3.0F),
  };
  const std::array<float, 2> c2 = {
      p0[0] + ((p1[0] - p0[0]) * (2.0F / 3.0F)),
      p0[1] + ((p1[1] - p0[1]) * (2.0F / 3.0F)),
  };

  SfntGpuCurveSegment segment{};
  segment.p0x = p0[0];
  segment.p0y = p0[1];
  segment.p1x = c1[0];
  segment.p1y = c1[1];
  segment.p2x = c2[0];
  segment.p2y = c2[1];
  segment.p3x = p1[0];
  segment.p3y = p1[1];
  segment.type = 1;
  segment.channelMask = 0x07;
  segment.contourSign = 1;
  return segment;
}

std::array<float, 2> subtractVec(const std::array<float, 2>& a, const std::array<float, 2>& b) {
  return {
      a[0] - b[0],
      a[1] - b[1],
  };
}

float dotVec(const std::array<float, 2>& a, const std::array<float, 2>& b) {
  return a[0] * b[0] + a[1] * b[1];
}

float lengthSquaredVec(const std::array<float, 2>& v) {
  return dotVec(v, v);
}

std::array<float, 2> normalizeVec(const std::array<float, 2>& v) {
  const float lengthSquared = lengthSquaredVec(v);
  if (lengthSquared <= 1.0e-10F) {
    return {0.0F, 0.0F};
  }

  const float inverseLength = 1.0F / std::sqrt(lengthSquared);
  return {
      v[0] * inverseLength,
      v[1] * inverseLength,
  };
}

float signedAreaOfPolygon(const std::vector<std::array<float, 2>>& vertices) {
  if (vertices.size() < 3U) {
    return 0.0F;
  }

  float area = 0.0F;
  for (std::size_t i = 0U; i < vertices.size(); ++i) {
    const auto& current = vertices[i];
    const auto& next = vertices[(i + 1U) % vertices.size()];
    area += current[0] * next[1] - next[0] * current[1];
  }
  return area * 0.5F;
}

bool isMsdfCorner(const std::array<float, 2>& prevTangent,
                  const std::array<float, 2>& nextTangent) {
  const auto prev = normalizeVec(prevTangent);
  const auto next = normalizeVec(nextTangent);
  if (lengthSquaredVec(prev) <= 1.0e-10F || lengthSquaredVec(next) <= 1.0e-10F) {
    return false;
  }

  const float cosine = std::clamp(dotVec(prev, next), -1.0F, 1.0F);
  return cosine < 0.55F;
}

std::uint8_t nextMsdfColor(std::uint8_t color) {
  switch (color) {
    case 0x01U:
      return 0x02U;
    case 0x02U:
      return 0x04U;
    default:
      return 0x01U;
  }
}

void assignMsdfEdgeColors(std::vector<GpuContourEdge>& edges) {
  if (edges.size() < 3U) {
    for (GpuContourEdge& edge : edges) {
      edge.segment.channelMask = 0x07;
    }
    return;
  }

  std::vector<bool> corners(edges.size(), false);
  std::size_t cornerCount = 0U;
  for (std::size_t i = 0U; i < edges.size(); ++i) {
    const std::size_t nextIndex = (i + 1U) % edges.size();
    corners[i] = isMsdfCorner(edges[i].endTangent, edges[nextIndex].startTangent);
    cornerCount += corners[i] ? 1U : 0U;
  }

  if (cornerCount < 2U) {
    for (GpuContourEdge& edge : edges) {
      edge.segment.channelMask = 0x07;
    }
    return;
  }

  std::size_t startEdge = 0U;
  while (startEdge < corners.size() && !corners[startEdge]) {
    ++startEdge;
  }
  startEdge = (startEdge + 1U) % edges.size();

  std::uint8_t currentColor = 0x01U;
  for (std::size_t step = 0U; step < edges.size(); ++step) {
    const std::size_t edgeIndex = (startEdge + step) % edges.size();
    edges[edgeIndex].segment.channelMask = currentColor;
    if (corners[edgeIndex]) {
      currentColor = nextMsdfColor(currentColor);
    }
  }
}

GpuContourEdge makeLinearEdge(const std::array<float, 2>& p0,
                              const std::array<float, 2>& p1) {
  GpuContourEdge edge{};
  edge.segment = makeLinearCubicSegment(p0, p1);
  edge.startPoint = p0;
  edge.endPoint = p1;
  const auto tangent = subtractVec(p1, p0);
  edge.startTangent = tangent;
  edge.endTangent = tangent;
  return edge;
}

GpuContourEdge makeQuadraticEdge(const std::array<float, 2>& p0,
                                 const std::array<float, 2>& p1,
                                 const std::array<float, 2>& p2) {
  GpuContourEdge edge{};
  edge.segment = makeQuadraticSegment(p0, p1, p2);
  edge.startPoint = p0;
  edge.endPoint = p2;
  edge.startTangent = subtractVec(p1, p0);
  edge.endTangent = subtractVec(p2, p1);
  return edge;
}

bool appendContourGpuSegments(const Contour& contour,
                              const GlyphRasterBounds& bounds,
                              float scale,
                              std::vector<SfntGpuCurveSegment>& outSegments) {
  if (contour.empty()) {
    return true;
  }

  const std::size_t count = contour.size();
  auto pointAt = [&](std::size_t index) -> const Point& {
    return contour[index % count];
  };

  std::array<float, 2> start{};
  std::size_t index = 0U;
  if (contour[0].onCurve) {
    start = normalizeGlyphPoint(contour[0].x, contour[0].y, bounds, scale);
    index = 1U;
  } else if (contour[count - 1U].onCurve) {
    start = normalizeGlyphPoint(contour[count - 1U].x, contour[count - 1U].y, bounds, scale);
    index = 0U;
  } else {
    start = normalizeGlyphPoint(
        (contour[count - 1U].x + contour[0].x) * 0.5F,
        (contour[count - 1U].y + contour[0].y) * 0.5F,
        bounds,
        scale);
    index = 0U;
  }

  std::array<float, 2> current = start;
  std::vector<GpuContourEdge> contourEdges;
  contourEdges.reserve(count + 1U);
  std::vector<std::array<float, 2>> contourVertices;
  contourVertices.reserve(count + 1U);
  contourVertices.push_back(start);
  std::size_t consumed = 0U;

  while (consumed < count) {
    const Point& p1 = pointAt(index);
    if (p1.onCurve) {
      const std::array<float, 2> next = normalizeGlyphPoint(p1.x, p1.y, bounds, scale);
      if (next != current) {
        contourEdges.push_back(makeLinearEdge(current, next));
        contourVertices.push_back(next);
        current = next;
      }
      index = (index + 1U) % count;
      ++consumed;
      continue;
    }

    const Point& p2 = pointAt(index + 1U);
    const std::array<float, 2> control = normalizeGlyphPoint(p1.x, p1.y, bounds, scale);
    if (p2.onCurve) {
      const std::array<float, 2> end = normalizeGlyphPoint(p2.x, p2.y, bounds, scale);
      contourEdges.push_back(makeQuadraticEdge(current, control, end));
      contourVertices.push_back(end);
      current = end;
      index = (index + 2U) % count;
      consumed += 2U;
      continue;
    }

    const std::array<float, 2> end = normalizeGlyphPoint(
        (p1.x + p2.x) * 0.5F,
        (p1.y + p2.y) * 0.5F,
        bounds,
        scale);
    contourEdges.push_back(makeQuadraticEdge(current, control, end));
    contourVertices.push_back(end);
    current = end;
    index = (index + 1U) % count;
    ++consumed;
  }

  if (contourEdges.empty()) {
    return true;
  }

  const float area = signedAreaOfPolygon(contourVertices);
  const std::int32_t contourSign = area < 0.0F ? -1 : 1;
  assignMsdfEdgeColors(contourEdges);
  for (GpuContourEdge& edge : contourEdges) {
    edge.segment.contourSign = contourSign;
    outSegments.push_back(edge.segment);
  }

  return true;
}

bool buildGpuCurveSegments(const GlyphOutline& outline,
                           const GlyphRasterBounds& bounds,
                           float scale,
                           std::vector<SfntGpuCurveSegment>& outSegments) {
  outSegments.clear();

  for (const Contour& contour : outline.contours) {
    if (!appendContourGpuSegments(contour, bounds, scale, outSegments)) {
      return false;
    }
  }

  return true;
}

bool packVectorGlyphsIntoAtlas(std::vector<GlyphVectorJob>& glyphs,
                               std::uint32_t& outWidth,
                               std::uint32_t& outHeight,
                               std::vector<SfntGpuCurveSegment>& outCurves,
                               std::vector<SfntGpuGlyphJob>& outJobs,
                               std::uint32_t& outMaxGlyphWidth,
                               std::uint32_t& outMaxGlyphHeight) {
  outWidth = 0U;
  outHeight = 0U;
  outMaxGlyphWidth = 0U;
  outMaxGlyphHeight = 0U;
  outCurves.clear();
  outJobs.clear();

  std::uint64_t totalArea = 0U;
  std::uint32_t maxGlyphWidth = 0U;
  for (const GlyphVectorJob& glyph : glyphs) {
    if (glyph.widthPx == 0U || glyph.heightPx == 0U || glyph.curves.empty()) {
      continue;
    }

    maxGlyphWidth = std::max(maxGlyphWidth, glyph.widthPx + 2U);
    outMaxGlyphWidth = std::max(outMaxGlyphWidth, glyph.widthPx);
    outMaxGlyphHeight = std::max(outMaxGlyphHeight, glyph.heightPx);
    totalArea += static_cast<std::uint64_t>(glyph.widthPx + 2U) *
                 static_cast<std::uint64_t>(glyph.heightPx + 2U);
  }

  if (totalArea == 0U) {
    outWidth = 1U;
    outHeight = 1U;
    return true;
  }

  std::uint32_t atlasWidth = 256U;
  while (atlasWidth < maxGlyphWidth) {
    atlasWidth *= 2U;
  }
  while (static_cast<std::uint64_t>(atlasWidth) * static_cast<std::uint64_t>(atlasWidth) < totalArea) {
    atlasWidth *= 2U;
    if (atlasWidth > kMaxAtlasDimension) {
      return false;
    }
  }

  atlasWidth = std::min(atlasWidth, kMaxAtlasDimension);

  std::uint32_t cursorX = 1U;
  std::uint32_t cursorY = 1U;
  std::uint32_t rowHeight = 0U;
  std::uint32_t atlasHeightUsed = 1U;

  for (GlyphVectorJob& glyph : glyphs) {
    if (glyph.widthPx == 0U || glyph.heightPx == 0U || glyph.curves.empty()) {
      glyph.metrics.s0 = 0.0F;
      glyph.metrics.t0 = 0.0F;
      glyph.metrics.s1 = 0.0F;
      glyph.metrics.t1 = 0.0F;
      continue;
    }

    if (cursorX + glyph.widthPx + 1U > atlasWidth) {
      cursorX = 1U;
      cursorY += rowHeight + 1U;
      rowHeight = 0U;
    }

    if (cursorY + glyph.heightPx + 1U > kMaxAtlasDimension) {
      return false;
    }

    glyph.atlasX = cursorX;
    glyph.atlasY = cursorY;
    glyph.metrics.s0 = static_cast<float>(cursorX) / static_cast<float>(atlasWidth);
    glyph.metrics.t0 = static_cast<float>(cursorY) / static_cast<float>(kMaxAtlasDimension);
    glyph.metrics.s1 = static_cast<float>(cursorX + glyph.widthPx) / static_cast<float>(atlasWidth);
    glyph.metrics.t1 = static_cast<float>(cursorY + glyph.heightPx) / static_cast<float>(kMaxAtlasDimension);

    cursorX += glyph.widthPx + 1U;
    rowHeight = std::max(rowHeight, glyph.heightPx);
    atlasHeightUsed = std::max(atlasHeightUsed, cursorY + glyph.heightPx + 1U);
  }

  std::uint32_t atlasHeight = 1U;
  while (atlasHeight < atlasHeightUsed) {
    atlasHeight *= 2U;
  }
  atlasHeight = std::min(atlasHeight, kMaxAtlasDimension);
  if (atlasHeight < atlasHeightUsed) {
    return false;
  }

  for (GlyphVectorJob& glyph : glyphs) {
    if (glyph.widthPx == 0U || glyph.heightPx == 0U || glyph.curves.empty()) {
      continue;
    }

    glyph.metrics.t0 *= static_cast<float>(kMaxAtlasDimension) / static_cast<float>(atlasHeight);
    glyph.metrics.t1 *= static_cast<float>(kMaxAtlasDimension) / static_cast<float>(atlasHeight);
  }

  outWidth = atlasWidth;
  outHeight = atlasHeight;

  for (std::size_t glyphIndex = 0U; glyphIndex < glyphs.size(); ++glyphIndex) {
    const GlyphVectorJob& glyph = glyphs[glyphIndex];
    if (glyph.widthPx == 0U || glyph.heightPx == 0U || glyph.curves.empty()) {
      continue;
    }

    const std::int32_t curveOffset = static_cast<std::int32_t>(outCurves.size());
    outCurves.insert(outCurves.end(), glyph.curves.begin(), glyph.curves.end());
    outJobs.push_back(SfntGpuGlyphJob{
        static_cast<std::int32_t>(glyph.atlasX),
        static_cast<std::int32_t>(glyph.atlasY),
        static_cast<std::int32_t>(glyph.widthPx),
        static_cast<std::int32_t>(glyph.heightPx),
        curveOffset,
        static_cast<std::int32_t>(glyph.curves.size()),
        static_cast<std::int32_t>(glyphIndex),
        0,
    });
  }

  return true;
}

bool readSimpleGlyph(const SfntFont& font,
                     std::size_t glyphOffset,
                     std::size_t glyphEnd,
                     std::int16_t contourCount,
                     GlyphOutline& outOutline) {
  std::size_t cursor = glyphOffset + 10U;

  std::vector<std::uint16_t> endPoints;
  endPoints.resize(static_cast<std::size_t>(contourCount));
  for (std::size_t i = 0U; i < endPoints.size(); ++i) {
    if (!readU16Be(font.bytes, cursor, endPoints[i])) {
      return false;
    }
    cursor += 2U;
  }

  if (endPoints.empty()) {
    outOutline.contours.clear();
    return true;
  }

  const std::size_t pointCount = static_cast<std::size_t>(endPoints.back()) + 1U;
  if (pointCount == 0U) {
    outOutline.contours.clear();
    return true;
  }

  std::uint16_t instructionLength = 0U;
  if (!readU16Be(font.bytes, cursor, instructionLength)) {
    return false;
  }
  cursor += 2U;

  if (cursor + instructionLength > glyphEnd) {
    return false;
  }
  cursor += instructionLength;

  std::vector<std::uint8_t> flags;
  flags.reserve(pointCount);
  while (flags.size() < pointCount) {
    if (cursor + 1U > glyphEnd) {
      return false;
    }

    const std::uint8_t flag = font.bytes[cursor++];
    flags.push_back(flag);

    if ((flag & 0x08U) == 0U) {
      continue;
    }

    if (cursor + 1U > glyphEnd) {
      return false;
    }

    const std::uint8_t repeatCount = font.bytes[cursor++];
    if (flags.size() + repeatCount > pointCount) {
      return false;
    }

    for (std::uint8_t r = 0U; r < repeatCount; ++r) {
      flags.push_back(flag);
    }
  }

  std::vector<std::int32_t> xs(pointCount, 0);
  std::vector<std::int32_t> ys(pointCount, 0);

  std::int32_t x = 0;
  for (std::size_t i = 0U; i < pointCount; ++i) {
    const std::uint8_t flag = flags[i];
    if ((flag & 0x02U) != 0U) {
      if (cursor + 1U > glyphEnd) {
        return false;
      }
      const std::int32_t dx = static_cast<std::int32_t>(font.bytes[cursor++]);
      x += ((flag & 0x10U) != 0U) ? dx : -dx;
    } else if ((flag & 0x10U) == 0U) {
      std::int16_t dx = 0;
      if (!readI16Be(font.bytes, cursor, dx)) {
        return false;
      }
      cursor += 2U;
      x += dx;
    }

    xs[i] = x;
  }

  std::int32_t y = 0;
  for (std::size_t i = 0U; i < pointCount; ++i) {
    const std::uint8_t flag = flags[i];
    if ((flag & 0x04U) != 0U) {
      if (cursor + 1U > glyphEnd) {
        return false;
      }
      const std::int32_t dy = static_cast<std::int32_t>(font.bytes[cursor++]);
      y += ((flag & 0x20U) != 0U) ? dy : -dy;
    } else if ((flag & 0x20U) == 0U) {
      std::int16_t dy = 0;
      if (!readI16Be(font.bytes, cursor, dy)) {
        return false;
      }
      cursor += 2U;
      y += dy;
    }

    ys[i] = y;
  }

  outOutline.contours.clear();
  outOutline.contours.reserve(endPoints.size());

  std::size_t start = 0U;
  for (std::size_t contourIndex = 0U; contourIndex < endPoints.size(); ++contourIndex) {
    const std::size_t end = static_cast<std::size_t>(endPoints[contourIndex]);
    if (end < start || end >= pointCount) {
      return false;
    }

    Contour contour;
    contour.reserve(end - start + 1U);
    for (std::size_t p = start; p <= end; ++p) {
      contour.push_back(Point{
          static_cast<float>(xs[p]),
          static_cast<float>(ys[p]),
          (flags[p] & 0x01U) != 0U,
      });
    }

    outOutline.contours.push_back(std::move(contour));
    start = end + 1U;
  }

  return true;
}

bool readGlyphOutlineRecursive(const SfntFont& font,
                               std::uint16_t glyphIndex,
                               GlyphOutline& outOutline,
                               int depth) {
  if (depth > 8) {
    return false;
  }

  std::size_t glyphOffset = 0U;
  std::size_t glyphEnd = 0U;
  if (!glyphDataRange(font, glyphIndex, glyphOffset, glyphEnd)) {
    return false;
  }

  if (glyphEnd <= glyphOffset) {
    outOutline = GlyphOutline{};
    return true;
  }

  if (glyphOffset + 10U > glyphEnd) {
    return false;
  }

  std::int16_t contourCount = 0;
  std::int16_t xMin = 0;
  std::int16_t yMin = 0;
  std::int16_t xMax = 0;
  std::int16_t yMax = 0;

  if (!readI16Be(font.bytes, glyphOffset + 0U, contourCount) ||
      !readI16Be(font.bytes, glyphOffset + 2U, xMin) ||
      !readI16Be(font.bytes, glyphOffset + 4U, yMin) ||
      !readI16Be(font.bytes, glyphOffset + 6U, xMax) ||
      !readI16Be(font.bytes, glyphOffset + 8U, yMax)) {
    return false;
  }

  outOutline.xMin = xMin;
  outOutline.yMin = yMin;
  outOutline.xMax = xMax;
  outOutline.yMax = yMax;

  if (contourCount >= 0) {
    return readSimpleGlyph(font, glyphOffset, glyphEnd, contourCount, outOutline);
  }

  std::vector<Contour> mergedContours;
  std::size_t cursor = glyphOffset + 10U;

  constexpr std::uint16_t kArgWords = 0x0001U;
  constexpr std::uint16_t kArgsAreXYValues = 0x0002U;
  constexpr std::uint16_t kWeHaveScale = 0x0008U;
  constexpr std::uint16_t kMoreComponents = 0x0020U;
  constexpr std::uint16_t kWeHaveXYScale = 0x0040U;
  constexpr std::uint16_t kWeHaveTwoByTwo = 0x0080U;

  std::uint16_t flags = 0U;

  do {
    std::uint16_t componentGlyphIndex = 0U;
    if (!readU16Be(font.bytes, cursor + 0U, flags) ||
        !readU16Be(font.bytes, cursor + 2U, componentGlyphIndex)) {
      return false;
    }
    cursor += 4U;

    if (componentGlyphIndex >= font.numGlyphs) {
      return false;
    }

    std::int32_t arg1 = 0;
    std::int32_t arg2 = 0;
    if ((flags & kArgWords) != 0U) {
      std::int16_t a = 0;
      std::int16_t b = 0;
      if (!readI16Be(font.bytes, cursor + 0U, a) || !readI16Be(font.bytes, cursor + 2U, b)) {
        return false;
      }
      arg1 = a;
      arg2 = b;
      cursor += 4U;
    } else {
      std::int8_t a = 0;
      std::int8_t b = 0;
      if (!readS8(font.bytes, cursor + 0U, a) || !readS8(font.bytes, cursor + 1U, b)) {
        return false;
      }
      arg1 = a;
      arg2 = b;
      cursor += 2U;
    }

    if ((flags & kArgsAreXYValues) == 0U) {
      return false;
    }

    float m00 = 1.0F;
    float m01 = 0.0F;
    float m10 = 0.0F;
    float m11 = 1.0F;

    if ((flags & kWeHaveScale) != 0U) {
      std::int16_t scale = 0;
      if (!readI16Be(font.bytes, cursor, scale)) {
        return false;
      }
      cursor += 2U;
      const float s = static_cast<float>(scale) / 16384.0F;
      m00 = s;
      m11 = s;
    } else if ((flags & kWeHaveXYScale) != 0U) {
      std::int16_t sx = 0;
      std::int16_t sy = 0;
      if (!readI16Be(font.bytes, cursor + 0U, sx) || !readI16Be(font.bytes, cursor + 2U, sy)) {
        return false;
      }
      cursor += 4U;
      m00 = static_cast<float>(sx) / 16384.0F;
      m11 = static_cast<float>(sy) / 16384.0F;
    } else if ((flags & kWeHaveTwoByTwo) != 0U) {
      std::int16_t a = 0;
      std::int16_t b = 0;
      std::int16_t c = 0;
      std::int16_t d = 0;
      if (!readI16Be(font.bytes, cursor + 0U, a) ||
          !readI16Be(font.bytes, cursor + 2U, b) ||
          !readI16Be(font.bytes, cursor + 4U, c) ||
          !readI16Be(font.bytes, cursor + 6U, d)) {
        return false;
      }
      cursor += 8U;
      m00 = static_cast<float>(a) / 16384.0F;
      m01 = static_cast<float>(b) / 16384.0F;
      m10 = static_cast<float>(c) / 16384.0F;
      m11 = static_cast<float>(d) / 16384.0F;
    }

    GlyphOutline component{};
    if (!readGlyphOutlineRecursive(font, componentGlyphIndex, component, depth + 1)) {
      return false;
    }

    for (Contour contour : component.contours) {
      for (Point& p : contour) {
        const float x = p.x;
        const float yv = p.y;
        p.x = m00 * x + m01 * yv + static_cast<float>(arg1);
        p.y = m10 * x + m11 * yv + static_cast<float>(arg2);
      }
      mergedContours.push_back(std::move(contour));
    }

  } while ((flags & kMoreComponents) != 0U);

  outOutline.contours = std::move(mergedContours);
  return true;
}

void distanceTransform1D(const std::vector<float>& input, int count, std::vector<float>& output) {
  output.assign(static_cast<std::size_t>(count), kDistanceFieldInfinity);

  bool hasFinite = false;
  for (int i = 0; i < count; ++i) {
    if (input[static_cast<std::size_t>(i)] < kDistanceFieldInfinity * 0.5F) {
      hasFinite = true;
      break;
    }
  }
  if (!hasFinite) {
    return;
  }

  std::vector<int> vertices(static_cast<std::size_t>(count), 0);
  std::vector<float> intersections(static_cast<std::size_t>(count + 1), 0.0F);

  int hullSize = 0;
  vertices[0] = 0;
  intersections[0] = kDistanceFieldNegInfinity;
  intersections[1] = kDistanceFieldInfinity;

  for (int q = 1; q < count; ++q) {
    float intersection = 0.0F;
    while (true) {
      const int v = vertices[static_cast<std::size_t>(hullSize)];
      const float numerator =
          (input[static_cast<std::size_t>(q)] + static_cast<float>(q * q)) -
          (input[static_cast<std::size_t>(v)] + static_cast<float>(v * v));
      const float denominator = static_cast<float>(2 * (q - v));
      intersection = numerator / denominator;

      if (intersection > intersections[static_cast<std::size_t>(hullSize)] || hullSize == 0) {
        break;
      }
      --hullSize;
    }

    ++hullSize;
    vertices[static_cast<std::size_t>(hullSize)] = q;
    intersections[static_cast<std::size_t>(hullSize)] = intersection;
    intersections[static_cast<std::size_t>(hullSize + 1)] = kDistanceFieldInfinity;
  }

  int segment = 0;
  for (int q = 0; q < count; ++q) {
    while (intersections[static_cast<std::size_t>(segment + 1)] < static_cast<float>(q)) {
      ++segment;
    }

    const float delta = static_cast<float>(q - vertices[static_cast<std::size_t>(segment)]);
    output[static_cast<std::size_t>(q)] =
        (delta * delta) + input[static_cast<std::size_t>(vertices[static_cast<std::size_t>(segment)])];
  }
}

void buildSquaredDistanceField(const std::vector<std::uint8_t>& featureMask,
                               std::uint32_t width,
                               std::uint32_t height,
                               std::vector<float>& outDistances) {
  const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  outDistances.assign(pixelCount, kDistanceFieldInfinity);
  if (pixelCount == 0U) {
    return;
  }

  std::vector<float> intermediate(pixelCount, kDistanceFieldInfinity);
  std::vector<float> lineIn;
  std::vector<float> lineOut;

  const int widthInt = static_cast<int>(width);
  const int heightInt = static_cast<int>(height);

  lineIn.resize(static_cast<std::size_t>(widthInt));
  for (int y = 0; y < heightInt; ++y) {
    const std::size_t rowOffset = static_cast<std::size_t>(y) * static_cast<std::size_t>(widthInt);
    for (int x = 0; x < widthInt; ++x) {
      const std::size_t index = rowOffset + static_cast<std::size_t>(x);
      lineIn[static_cast<std::size_t>(x)] = featureMask[index] != 0U ? 0.0F : kDistanceFieldInfinity;
    }

    distanceTransform1D(lineIn, widthInt, lineOut);
    for (int x = 0; x < widthInt; ++x) {
      intermediate[rowOffset + static_cast<std::size_t>(x)] = lineOut[static_cast<std::size_t>(x)];
    }
  }

  lineIn.resize(static_cast<std::size_t>(heightInt));
  for (int x = 0; x < widthInt; ++x) {
    for (int y = 0; y < heightInt; ++y) {
      lineIn[static_cast<std::size_t>(y)] =
          intermediate[static_cast<std::size_t>(y) * static_cast<std::size_t>(widthInt) +
                       static_cast<std::size_t>(x)];
    }

    distanceTransform1D(lineIn, heightInt, lineOut);
    for (int y = 0; y < heightInt; ++y) {
      outDistances[static_cast<std::size_t>(y) * static_cast<std::size_t>(widthInt) +
                   static_cast<std::size_t>(x)] = lineOut[static_cast<std::size_t>(y)];
    }
  }
}

void convertCoverageToSignedDistanceField(std::vector<std::uint8_t>& alpha,
                                          std::uint32_t width,
                                          std::uint32_t height,
                                          int spreadPx) {
  if (alpha.empty() || width == 0U || height == 0U) {
    return;
  }

  const int normalizedSpread = std::clamp(spreadPx, 2, 64);
  const float maxDistance = static_cast<float>(normalizedSpread);
  const float maxDistanceSquared = maxDistance * maxDistance;

  std::vector<std::uint8_t> inside(alpha.size(), 0U);
  std::vector<std::uint8_t> outside(alpha.size(), 0U);
  for (std::size_t i = 0U; i < alpha.size(); ++i) {
    const bool isInside = alpha[i] >= 128U;
    inside[i] = isInside ? 1U : 0U;
    outside[i] = isInside ? 0U : 1U;
  }

  std::vector<float> distanceToInsideSquared;
  std::vector<float> distanceToOutsideSquared;
  buildSquaredDistanceField(inside, width, height, distanceToInsideSquared);
  buildSquaredDistanceField(outside, width, height, distanceToOutsideSquared);

  std::vector<std::uint8_t> sdf(alpha.size(), 0U);
  for (std::size_t i = 0U; i < alpha.size(); ++i) {
    const bool isInside = inside[i] != 0U;
    const float oppositeDistanceSquared = isInside ? distanceToOutsideSquared[i] : distanceToInsideSquared[i];
    const float clampedDistance = std::sqrt(std::min(oppositeDistanceSquared, maxDistanceSquared));
    const float signedDistance = isInside ? clampedDistance : -clampedDistance;

    const float normalized = std::clamp(
        0.5F + (signedDistance / maxDistance) * 0.5F,
        0.0F,
        1.0F);
    sdf[i] = static_cast<std::uint8_t>(std::lround(normalized * 255.0F));
  }

  alpha = std::move(sdf);
}

bool rasterizeGlyphOutline(const GlyphOutline& outline,
                           float scale,
                           std::uint16_t advanceWidthUnits,
                           const SfntAtlasBuildOptions& buildOptions,
                           GlyphBitmap& outGlyph) {
  outGlyph = GlyphBitmap{};
  outGlyph.metrics.xAdvance = static_cast<float>(advanceWidthUnits) * scale;

  if (outline.contours.empty() || outline.xMin >= outline.xMax || outline.yMin >= outline.yMax) {
    return true;
  }

  std::vector<Polyline> polylines;
  if (!buildFlattenedPolylines(outline, scale, polylines) || polylines.empty()) {
    return false;
  }

  const bool useDistanceField =
      buildOptions.rasterMode == SfntAtlasRasterMode::kSignedDistanceField ||
      buildOptions.rasterMode == SfntAtlasRasterMode::kMultiChannelSignedDistanceField;

  constexpr int kCoverageSupersample = 4;
  constexpr int kDistanceFieldSupersample = 1;

  const int supersample = useDistanceField ? kDistanceFieldSupersample : kCoverageSupersample;
  const float clampedScale = std::max(scale, 0.01F);
  const float inverseScale = 1.0F / clampedScale;

  GlyphRasterBounds bounds{};
  if (!computeGlyphRasterBounds(outline, scale, buildOptions, bounds)) {
    return false;
  }

  const float leftUnits = bounds.leftUnits;
  const float topUnits = bounds.topUnits;
  const std::uint32_t widthPx = bounds.widthPx;
  const std::uint32_t heightPx = bounds.heightPx;

  outGlyph.widthPx = widthPx;
  outGlyph.heightPx = heightPx;
  outGlyph.alpha.resize(static_cast<std::size_t>(widthPx) * static_cast<std::size_t>(heightPx), 0U);

  const int totalSamples = supersample * supersample;
  for (std::uint32_t py = 0U; py < heightPx; ++py) {
    for (std::uint32_t px = 0U; px < widthPx; ++px) {
      int insideSamples = 0;
      if (supersample == 1) {
        const float sampleX = static_cast<float>(px) + 0.5F;
        const float sampleY = static_cast<float>(py) + 0.5F;
        const float glyphX = leftUnits + sampleX * inverseScale;
        const float glyphY = topUnits - sampleY * inverseScale;
        insideSamples = pointInsideEvenOdd(glyphX, glyphY, polylines) ? 1 : 0;
      } else {
        for (int sy = 0; sy < supersample; ++sy) {
          for (int sx = 0; sx < supersample; ++sx) {
            const float sampleX = static_cast<float>(px) + (static_cast<float>(sx) + 0.5F) /
                                                           static_cast<float>(supersample);
            const float sampleY = static_cast<float>(py) + (static_cast<float>(sy) + 0.5F) /
                                                           static_cast<float>(supersample);

            const float glyphX = leftUnits + sampleX * inverseScale;
            const float glyphY = topUnits - sampleY * inverseScale;

            if (pointInsideEvenOdd(glyphX, glyphY, polylines)) {
              ++insideSamples;
            }
          }
        }
      }

      const std::uint8_t alpha = static_cast<std::uint8_t>(
          (insideSamples * 255 + (totalSamples / 2)) / totalSamples);
      outGlyph.alpha[static_cast<std::size_t>(py) * static_cast<std::size_t>(widthPx) + px] = alpha;
    }
  }

  outGlyph.metrics.xOffset = leftUnits * scale;
  outGlyph.metrics.yOffset = -topUnits * scale;
  outGlyph.metrics.width = static_cast<float>(widthPx);
  outGlyph.metrics.height = static_cast<float>(heightPx);

  if (buildOptions.rasterMode == SfntAtlasRasterMode::kSignedDistanceField ||
      buildOptions.rasterMode == SfntAtlasRasterMode::kMultiChannelSignedDistanceField) {
    convertCoverageToSignedDistanceField(
        outGlyph.alpha,
        outGlyph.widthPx,
        outGlyph.heightPx,
        buildOptions.sdfSpreadPx);
  }

  if (buildOptions.rasterMode == SfntAtlasRasterMode::kMultiChannelSignedDistanceField) {
    std::vector<MsdfEdgeSegment> msdfSegments;
    if (!buildMsdfEdgeSegments(polylines, msdfSegments)) {
      return false;
    }

    std::vector<MsdfPreparedSegment> preparedMsdfSegments;
    buildPreparedMsdfSegments(msdfSegments, preparedMsdfSegments);

    const float spreadPx = static_cast<float>(std::clamp(buildOptions.sdfSpreadPx, 2, 64));
    const float spreadSquared = spreadPx * spreadPx;

    outGlyph.rgb.assign(static_cast<std::size_t>(widthPx) * static_cast<std::size_t>(heightPx) * 3U, 127U);

    for (std::uint32_t py = 0U; py < heightPx; ++py) {
      for (std::uint32_t px = 0U; px < widthPx; ++px) {
        const float sampleX = static_cast<float>(px) + 0.5F;
        const float sampleY = static_cast<float>(py) + 0.5F;
        const float glyphX = leftUnits + sampleX * inverseScale;
        const float glyphY = topUnits - sampleY * inverseScale;

        const std::size_t glyphPixelIndex =
            static_cast<std::size_t>(py) * static_cast<std::size_t>(widthPx) + static_cast<std::size_t>(px);
        const bool inside = outGlyph.alpha[glyphPixelIndex] >= 128U;

        std::array<float, 3> nearestDistanceSquared = {
            spreadSquared,
            spreadSquared,
            spreadSquared,
        };

        for (const MsdfPreparedSegment& segment : preparedMsdfSegments) {
          const float lowerBoundSquared = pointToAabbDistanceSquared(
              glyphX,
              glyphY,
              segment.xMin,
              segment.xMax,
              segment.yMin,
              segment.yMax);

          bool shouldEvaluate = false;
          if ((segment.channelMask & 0x01U) != 0U && lowerBoundSquared < nearestDistanceSquared[0]) {
            shouldEvaluate = true;
          }
          if ((segment.channelMask & 0x02U) != 0U && lowerBoundSquared < nearestDistanceSquared[1]) {
            shouldEvaluate = true;
          }
          if ((segment.channelMask & 0x04U) != 0U && lowerBoundSquared < nearestDistanceSquared[2]) {
            shouldEvaluate = true;
          }

          if (!shouldEvaluate) {
            continue;
          }

          const float distanceSquared = pointToPreparedSegmentDistanceSquared(glyphX, glyphY, segment);
          if ((segment.channelMask & 0x01U) != 0U) {
            nearestDistanceSquared[0] = std::min(nearestDistanceSquared[0], distanceSquared);
          }
          if ((segment.channelMask & 0x02U) != 0U) {
            nearestDistanceSquared[1] = std::min(nearestDistanceSquared[1], distanceSquared);
          }
          if ((segment.channelMask & 0x04U) != 0U) {
            nearestDistanceSquared[2] = std::min(nearestDistanceSquared[2], distanceSquared);
          }
        }

        const float nearestDistanceR = std::sqrt(nearestDistanceSquared[0]);
        const float nearestDistanceG = std::sqrt(nearestDistanceSquared[1]);
        const float nearestDistanceB = std::sqrt(nearestDistanceSquared[2]);
        const float sign = inside ? 1.0F : -1.0F;
        const std::size_t dst =
            (static_cast<std::size_t>(py) * static_cast<std::size_t>(widthPx) + static_cast<std::size_t>(px)) * 3U;
        outGlyph.rgb[dst + 0U] =
            encodeSignedDistanceToByte(sign * nearestDistanceR, spreadPx);
        outGlyph.rgb[dst + 1U] =
            encodeSignedDistanceToByte(sign * nearestDistanceG, spreadPx);
        outGlyph.rgb[dst + 2U] =
            encodeSignedDistanceToByte(sign * nearestDistanceB, spreadPx);
      }
    }
  }

  return true;
}

bool parseSfntFontUncached(const std::filesystem::path& fontPath, SfntFont& outFont, std::string& outError) {
  outFont = SfntFont{};

  if (!readBinaryFile(fontPath, outFont.bytes)) {
    outError = "unable to read font file";
    return false;
  }

  std::unordered_map<std::uint32_t, TableRecord> tables;
  if (!parseTableDirectory(outFont.bytes, tables, outFont.sfntVersion)) {
    outError = "invalid sfnt table directory";
    return false;
  }

  const auto findTable = [&](std::uint32_t tag, TableRecord& outRecord) -> bool {
    const auto it = tables.find(tag);
    if (it == tables.end()) {
      return false;
    }
    outRecord = it->second;
    return true;
  };

  const bool hasHead = findTable(makeTag('h', 'e', 'a', 'd'), outFont.head);
  const bool hasHhea = findTable(makeTag('h', 'h', 'e', 'a'), outFont.hhea);
  const bool hasMaxp = findTable(makeTag('m', 'a', 'x', 'p'), outFont.maxp);
  const bool hasHmtx = findTable(makeTag('h', 'm', 't', 'x'), outFont.hmtx);
  findTable(makeTag('k', 'e', 'r', 'n'), outFont.kern);
  const bool hasCmap = findTable(makeTag('c', 'm', 'a', 'p'), outFont.cmap);
  const bool hasLoca = findTable(makeTag('l', 'o', 'c', 'a'), outFont.loca);
  const bool hasGlyf = findTable(makeTag('g', 'l', 'y', 'f'), outFont.glyf);
  const bool hasCff = tables.find(makeTag('C', 'F', 'F', ' ')) != tables.end() ||
                      tables.find(makeTag('C', 'F', 'F', '2')) != tables.end();

  if (!(hasHead && hasHhea && hasMaxp && hasHmtx && hasCmap)) {
    outError = "font is missing required sfnt tables";
    return false;
  }

  if (!(hasLoca && hasGlyf)) {
    if (hasCff) {
      outError = "otf cff outlines are not supported yet";
    } else {
      outError = "font outlines are not available";
    }
    return false;
  }

  if (!parseHeadTable(outFont) ||
      !parseHheaTable(outFont) ||
      !parseMaxpTable(outFont) ||
      !validateHorizontalMetricsTable(outFont) ||
      !validateLocaTable(outFont) ||
      !parseCmap(outFont.bytes, outFont.cmap, outFont.cmapLookup)) {
    outError = "font table validation failed";
    return false;
  }

  return true;
}

bool getParsedSfntFont(const std::filesystem::path& fontPath,
                       std::shared_ptr<SfntFont>& outFont,
                       std::string& outError) {
  std::error_code ec;
  const auto lastWriteTime = std::filesystem::last_write_time(fontPath, ec);
  if (ec) {
    outError = "unable to stat font file";
    return false;
  }

  auto& cache = sfntFontCache();
  std::uint64_t& accessCounter = sfntFontCacheAccessCounter();
  const std::uint64_t accessTick = ++accessCounter;
  const std::string key = normalizedPathKey(fontPath);

  const auto it = cache.find(key);
  if (it != cache.end() && it->second.font && it->second.lastWriteTime == lastWriteTime) {
    it->second.lastAccessTick = accessTick;
    outFont = it->second.font;
    return true;
  }

  auto parsedFont = std::make_shared<SfntFont>();
  if (!parseSfntFontUncached(fontPath, *parsedFont, outError)) {
    return false;
  }

  cache[key] = SfntFontCacheEntry{lastWriteTime, parsedFont, accessTick};
  trimSfntFontCacheIfNeeded();
  outFont = std::move(parsedFont);
  return true;
}

bool packGlyphsIntoAtlas(std::vector<GlyphBitmap>& glyphs,
                         std::uint32_t& outWidth,
                         std::uint32_t& outHeight,
                         std::vector<std::uint8_t>& outRgba) {
  outWidth = 0U;
  outHeight = 0U;
  outRgba.clear();

  std::uint64_t totalArea = 0U;
  std::uint32_t maxGlyphWidth = 0U;
  for (const GlyphBitmap& glyph : glyphs) {
    if (glyph.widthPx == 0U || glyph.heightPx == 0U || glyph.alpha.empty()) {
      continue;
    }

    maxGlyphWidth = std::max(maxGlyphWidth, glyph.widthPx + 2U);
    totalArea += static_cast<std::uint64_t>(glyph.widthPx + 2U) *
                 static_cast<std::uint64_t>(glyph.heightPx + 2U);
  }

  if (totalArea == 0U) {
    outWidth = 1U;
    outHeight = 1U;
    outRgba = {255U, 255U, 255U, 0U};
    return true;
  }

  std::uint32_t atlasWidth = 256U;
  while (atlasWidth < maxGlyphWidth) {
    atlasWidth *= 2U;
  }
  while (static_cast<std::uint64_t>(atlasWidth) * static_cast<std::uint64_t>(atlasWidth) < totalArea) {
    atlasWidth *= 2U;
    if (atlasWidth > kMaxAtlasDimension) {
      return false;
    }
  }

  atlasWidth = std::min(atlasWidth, kMaxAtlasDimension);

  std::uint32_t cursorX = 1U;
  std::uint32_t cursorY = 1U;
  std::uint32_t rowHeight = 0U;
  std::uint32_t atlasHeightUsed = 1U;

  for (GlyphBitmap& glyph : glyphs) {
    if (glyph.widthPx == 0U || glyph.heightPx == 0U || glyph.alpha.empty()) {
      glyph.metrics.s0 = 0.0F;
      glyph.metrics.t0 = 0.0F;
      glyph.metrics.s1 = 0.0F;
      glyph.metrics.t1 = 0.0F;
      continue;
    }

    if (cursorX + glyph.widthPx + 1U > atlasWidth) {
      cursorX = 1U;
      cursorY += rowHeight + 1U;
      rowHeight = 0U;
    }

    if (cursorY + glyph.heightPx + 1U > kMaxAtlasDimension) {
      return false;
    }

    glyph.metrics.s0 = static_cast<float>(cursorX) / static_cast<float>(atlasWidth);
    glyph.metrics.t0 = static_cast<float>(cursorY) / static_cast<float>(kMaxAtlasDimension);
    glyph.metrics.s1 = static_cast<float>(cursorX + glyph.widthPx) / static_cast<float>(atlasWidth);
    glyph.metrics.t1 = static_cast<float>(cursorY + glyph.heightPx) / static_cast<float>(kMaxAtlasDimension);

    cursorX += glyph.widthPx + 1U;
    rowHeight = std::max(rowHeight, glyph.heightPx);
    atlasHeightUsed = std::max(atlasHeightUsed, cursorY + glyph.heightPx + 1U);
  }

  std::uint32_t atlasHeight = 1U;
  while (atlasHeight < atlasHeightUsed) {
    atlasHeight *= 2U;
  }
  atlasHeight = std::min(atlasHeight, kMaxAtlasDimension);
  if (atlasHeight < atlasHeightUsed) {
    return false;
  }

  for (GlyphBitmap& glyph : glyphs) {
    if (glyph.widthPx == 0U || glyph.heightPx == 0U || glyph.alpha.empty()) {
      continue;
    }

    glyph.metrics.t0 *= static_cast<float>(kMaxAtlasDimension) / static_cast<float>(atlasHeight);
    glyph.metrics.t1 *= static_cast<float>(kMaxAtlasDimension) / static_cast<float>(atlasHeight);
  }

  outWidth = atlasWidth;
  outHeight = atlasHeight;
  outRgba.assign(static_cast<std::size_t>(atlasWidth) * static_cast<std::size_t>(atlasHeight) * 4U, 0U);

  cursorX = 1U;
  cursorY = 1U;
  rowHeight = 0U;

  for (GlyphBitmap& glyph : glyphs) {
    if (glyph.widthPx == 0U || glyph.heightPx == 0U || glyph.alpha.empty()) {
      continue;
    }

    if (cursorX + glyph.widthPx + 1U > atlasWidth) {
      cursorX = 1U;
      cursorY += rowHeight + 1U;
      rowHeight = 0U;
    }

    for (std::uint32_t y = 0U; y < glyph.heightPx; ++y) {
      for (std::uint32_t x = 0U; x < glyph.widthPx; ++x) {
        const std::size_t srcIndex = static_cast<std::size_t>(y) * static_cast<std::size_t>(glyph.widthPx) + x;
        const std::size_t dstIndex = ((static_cast<std::size_t>(cursorY + y) * static_cast<std::size_t>(atlasWidth)) +
                                      static_cast<std::size_t>(cursorX + x)) * 4U;
        const std::uint8_t alpha = glyph.alpha[srcIndex];
        if (glyph.rgb.size() == glyph.alpha.size() * 3U) {
          const std::size_t rgbIndex = srcIndex * 3U;
          outRgba[dstIndex + 0U] = glyph.rgb[rgbIndex + 0U];
          outRgba[dstIndex + 1U] = glyph.rgb[rgbIndex + 1U];
          outRgba[dstIndex + 2U] = glyph.rgb[rgbIndex + 2U];
        } else {
          outRgba[dstIndex + 0U] = 255U;
          outRgba[dstIndex + 1U] = 255U;
          outRgba[dstIndex + 2U] = 255U;
        }
        outRgba[dstIndex + 3U] = alpha;
      }
    }

    cursorX += glyph.widthPx + 1U;
    rowHeight = std::max(rowHeight, glyph.heightPx);
  }

  return true;
}

}  // namespace

bool buildSfntAtlasFromFile(const std::filesystem::path& fontPath,
                            float bakePixelHeight,
                            const std::vector<int>& codepoints,
                            const SfntAtlasBuildOptions& buildOptions,
                            SfntAtlasResult& outResult) {
  outResult = SfntAtlasResult{};

  if (codepoints.empty() || bakePixelHeight <= 0.0F) {
    outResult.error = "invalid atlas build request";
    return false;
  }

  std::shared_ptr<SfntFont> font;
  if (!getParsedSfntFont(fontPath, font, outResult.error) || !font) {
    return false;
  }

  const float unitsPerEm = static_cast<float>(font->unitsPerEm);
  const float scale = bakePixelHeight / std::max(1.0F, unitsPerEm);

  std::uint16_t fallbackGlyphIndex = 0U;
  bool fallbackGlyphValid = glyphIndexForCodepoint(*font, static_cast<std::uint32_t>('?'), fallbackGlyphIndex);

  std::vector<GlyphBitmap> glyphBitmaps;
  glyphBitmaps.resize(codepoints.size());

  outResult.codepointToGlyphIndex.clear();
  outResult.codepointToGlyphIndex.reserve(codepoints.size());
  outResult.fontGlyphIndices.assign(codepoints.size(), 0U);

  for (std::size_t i = 0U; i < codepoints.size(); ++i) {
    const int cpInt = codepoints[i];
    if (cpInt < 0 || cpInt > 0x10FFFF) {
      continue;
    }

    const char32_t codepoint = static_cast<char32_t>(cpInt);

    std::uint16_t glyphIndex = 0U;
    if (!glyphIndexForCodepoint(*font, static_cast<std::uint32_t>(cpInt), glyphIndex)) {
      outResult.error = "cmap lookup failed";
      return false;
    }

    if (glyphIndex == 0U && cpInt != static_cast<int>('?') && fallbackGlyphValid) {
      glyphIndex = fallbackGlyphIndex;
      outResult.partial = true;
    }

    outResult.fontGlyphIndices[i] = glyphIndex;

    std::uint16_t advanceWidthUnits = 0U;
    std::int16_t leftSideBearing = 0;
    if (!glyphHorizontalMetrics(*font, glyphIndex, advanceWidthUnits, leftSideBearing)) {
      outResult.error = "hmtx lookup failed";
      return false;
    }

    GlyphBitmap glyph{};
    glyph.metrics.xAdvance = static_cast<float>(advanceWidthUnits) * scale;

    GlyphOutline outline{};
    if (!readGlyphOutlineRecursive(*font, glyphIndex, outline, 0)) {
      outResult.partial = true;
      glyph.metrics.xOffset = static_cast<float>(leftSideBearing) * scale;
      glyphBitmaps[i] = std::move(glyph);
      outResult.codepointToGlyphIndex[codepoint] = static_cast<int>(i);
      continue;
    }

    if (!rasterizeGlyphOutline(outline, scale, advanceWidthUnits, buildOptions, glyph)) {
      outResult.error = "glyph rasterization failed";
      return false;
    }

    glyphBitmaps[i] = std::move(glyph);
    outResult.codepointToGlyphIndex[codepoint] = static_cast<int>(i);
  }

  if (!buildAtlasKerningPairs(*font, outResult.fontGlyphIndices, scale, outResult.kerningPairsPx, outResult.error)) {
    return false;
  }

  std::uint32_t atlasWidth = 0U;
  std::uint32_t atlasHeight = 0U;
  std::vector<std::uint8_t> atlasRgba;
  if (!packGlyphsIntoAtlas(glyphBitmaps, atlasWidth, atlasHeight, atlasRgba)) {
    outResult.error = "glyph atlas packing failed";
    return false;
  }

  outResult.glyphs.resize(glyphBitmaps.size());
  for (std::size_t i = 0U; i < glyphBitmaps.size(); ++i) {
    outResult.glyphs[i] = glyphBitmaps[i].metrics;
  }

  outResult.atlasWidth = atlasWidth;
  outResult.atlasHeight = atlasHeight;
  outResult.rgba = std::move(atlasRgba);

  outResult.bakePixelHeight = bakePixelHeight;
  outResult.ascentPx = static_cast<float>(font->ascent) * scale;
  outResult.descentPx = static_cast<float>(font->descent) * scale;
  outResult.lineGapPx = static_cast<float>(font->lineGap) * scale;

  outResult.success = true;
  return true;
}

bool buildSfntGpuAtlasFromFile(const std::filesystem::path& fontPath,
                               float bakePixelHeight,
                               const std::vector<int>& codepoints,
                               const SfntAtlasBuildOptions& buildOptions,
                               SfntGpuAtlasResult& outResult) {
  outResult = SfntGpuAtlasResult{};

  if (codepoints.empty() || bakePixelHeight <= 0.0F) {
    outResult.error = "invalid gpu atlas build request";
    return false;
  }

  if (buildOptions.rasterMode == SfntAtlasRasterMode::kCoverage) {
    outResult.error = "coverage atlases are not GPU-generated";
    return false;
  }

  std::shared_ptr<SfntFont> font;
  if (!getParsedSfntFont(fontPath, font, outResult.error) || !font) {
    return false;
  }

  const float unitsPerEm = static_cast<float>(font->unitsPerEm);
  const float scale = bakePixelHeight / std::max(1.0F, unitsPerEm);

  std::uint16_t fallbackGlyphIndex = 0U;
  const bool fallbackGlyphValid = glyphIndexForCodepoint(*font, static_cast<std::uint32_t>('?'), fallbackGlyphIndex);

  std::vector<GlyphVectorJob> glyphJobs;
  glyphJobs.resize(codepoints.size());

  outResult.codepointToGlyphIndex.clear();
  outResult.codepointToGlyphIndex.reserve(codepoints.size());
  outResult.fontGlyphIndices.assign(codepoints.size(), 0U);

  for (std::size_t i = 0U; i < codepoints.size(); ++i) {
    const int cpInt = codepoints[i];
    if (cpInt < 0 || cpInt > 0x10FFFF) {
      continue;
    }

    const char32_t codepoint = static_cast<char32_t>(cpInt);

    std::uint16_t glyphIndex = 0U;
    if (!glyphIndexForCodepoint(*font, static_cast<std::uint32_t>(cpInt), glyphIndex)) {
      outResult.error = "cmap lookup failed";
      return false;
    }

    if (glyphIndex == 0U && cpInt != static_cast<int>('?') && fallbackGlyphValid) {
      glyphIndex = fallbackGlyphIndex;
      outResult.partial = true;
    }

    outResult.fontGlyphIndices[i] = glyphIndex;

    std::uint16_t advanceWidthUnits = 0U;
    std::int16_t leftSideBearing = 0;
    if (!glyphHorizontalMetrics(*font, glyphIndex, advanceWidthUnits, leftSideBearing)) {
      outResult.error = "hmtx lookup failed";
      return false;
    }

    GlyphVectorJob glyph{};
    glyph.metrics.xAdvance = static_cast<float>(advanceWidthUnits) * scale;

    GlyphOutline outline{};
    if (!readGlyphOutlineRecursive(*font, glyphIndex, outline, 0)) {
      outResult.partial = true;
      glyph.metrics.xOffset = static_cast<float>(leftSideBearing) * scale;
      glyphJobs[i] = std::move(glyph);
      outResult.codepointToGlyphIndex[codepoint] = static_cast<int>(i);
      continue;
    }

    GlyphRasterBounds bounds{};
    if (!computeGlyphRasterBounds(outline, scale, buildOptions, bounds)) {
      outResult.error = "gpu glyph bounds computation failed";
      return false;
    }

    glyph.metrics.xOffset = bounds.leftUnits * scale;
    glyph.metrics.yOffset = -bounds.topUnits * scale;
    glyph.metrics.width = static_cast<float>(bounds.widthPx);
    glyph.metrics.height = static_cast<float>(bounds.heightPx);
    glyph.widthPx = bounds.widthPx;
    glyph.heightPx = bounds.heightPx;

    if (!buildGpuCurveSegments(outline, bounds, scale, glyph.curves)) {
      outResult.error = "gpu curve extraction failed";
      return false;
    }

    glyphJobs[i] = std::move(glyph);
    outResult.codepointToGlyphIndex[codepoint] = static_cast<int>(i);
  }

  if (!buildAtlasKerningPairs(*font, outResult.fontGlyphIndices, scale, outResult.kerningPairsPx, outResult.error)) {
    return false;
  }

  std::uint32_t atlasWidth = 0U;
  std::uint32_t atlasHeight = 0U;
  std::uint32_t maxGlyphWidth = 0U;
  std::uint32_t maxGlyphHeight = 0U;
  std::vector<SfntGpuCurveSegment> curves;
  std::vector<SfntGpuGlyphJob> jobs;
  if (!packVectorGlyphsIntoAtlas(
          glyphJobs,
          atlasWidth,
          atlasHeight,
          curves,
          jobs,
          maxGlyphWidth,
          maxGlyphHeight)) {
    outResult.error = "gpu glyph atlas packing failed";
    return false;
  }

  outResult.glyphs.resize(glyphJobs.size());
  for (std::size_t i = 0U; i < glyphJobs.size(); ++i) {
    outResult.glyphs[i] = glyphJobs[i].metrics;
  }

  outResult.atlasWidth = atlasWidth;
  outResult.atlasHeight = atlasHeight;
  outResult.maxGlyphWidth = maxGlyphWidth;
  outResult.maxGlyphHeight = maxGlyphHeight;
  outResult.curves = std::move(curves);
  outResult.jobs = std::move(jobs);
  outResult.bakePixelHeight = bakePixelHeight;
  outResult.ascentPx = static_cast<float>(font->ascent) * scale;
  outResult.descentPx = static_cast<float>(font->descent) * scale;
  outResult.lineGapPx = static_cast<float>(font->lineGap) * scale;
  outResult.success = true;
  return true;
}

std::size_t sfntFontCacheEntryCountForTesting() {
  return sfntFontCache().size();
}

std::size_t sfntFontCacheMaxEntriesForTesting() {
  return kSfntFontCacheMaxEntries;
}

void clearSfntFontCacheForTesting() {
  sfntFontCache().clear();
  sfntFontCacheAccessCounter() = 0U;
}

}  // namespace volt::io::detail
