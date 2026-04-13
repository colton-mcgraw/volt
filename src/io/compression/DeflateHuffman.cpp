#include "DeflateHuffman.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace volt::io::compression::detail {
namespace {

constexpr std::uint8_t kDeflateMaxBits = 15U;

bool buildDeflateHuffmanTable(const std::vector<std::uint8_t>& bitLengths,
                              DeflateHuffmanTable& outTable,
                              std::string& error) {
  return buildCanonicalHuffmanTable(
      bitLengths,
      kDeflateMaxBits,
      false,
      HuffmanBitOrder::kLsbFirst,
      0U,
      outTable,
      error);
}

}  // namespace

bool decodeDeflateHuffmanSymbol(DeflateBitReader& reader,
                                const DeflateHuffmanTable& table,
                                int& symbol) {
  symbol = -1;
  std::uint16_t decoded = 0U;
  if (!decodeCanonicalHuffmanSymbol(reader, table, decoded)) {
    return false;
  }

  symbol = static_cast<int>(decoded);
  return true;
}

bool buildFixedDeflateHuffmanTables(DeflateHuffmanTable& litLenTable,
                                    DeflateHuffmanTable& distanceTable,
                                    std::string& error) {
  std::vector<std::uint8_t> litLenLengths(288U, 0U);
  for (std::size_t i = 0U; i <= 143U; ++i) {
    litLenLengths[i] = 8U;
  }
  for (std::size_t i = 144U; i <= 255U; ++i) {
    litLenLengths[i] = 9U;
  }
  for (std::size_t i = 256U; i <= 279U; ++i) {
    litLenLengths[i] = 7U;
  }
  for (std::size_t i = 280U; i <= 287U; ++i) {
    litLenLengths[i] = 8U;
  }

  std::vector<std::uint8_t> distanceLengths(32U, 5U);

  if (!buildDeflateHuffmanTable(litLenLengths, litLenTable, error)) {
    return false;
  }
  if (!buildDeflateHuffmanTable(distanceLengths, distanceTable, error)) {
    return false;
  }

  return true;
}

bool buildDynamicDeflateHuffmanTables(DeflateBitReader& reader,
                                      DeflateHuffmanTable& litLenTable,
                                      DeflateHuffmanTable& distanceTable,
                                      std::string& error) {
  std::uint32_t hlitBits = 0U;
  std::uint32_t hdistBits = 0U;
  std::uint32_t hclenBits = 0U;
  if (!reader.readBits(5U, hlitBits) || !reader.readBits(5U, hdistBits) ||
      !reader.readBits(4U, hclenBits)) {
    error = "dynamic deflate header truncated";
    return false;
  }

  const std::size_t hlit = static_cast<std::size_t>(hlitBits) + 257U;
  const std::size_t hdist = static_cast<std::size_t>(hdistBits) + 1U;
  const std::size_t hclen = static_cast<std::size_t>(hclenBits) + 4U;

  constexpr std::array<int, 19> kCodeLengthOrder = {
      16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
      11, 4, 12, 3, 13, 2, 14, 1, 15,
  };

  std::vector<std::uint8_t> codeLengthLengths(19U, 0U);
  for (std::size_t i = 0U; i < hclen; ++i) {
    std::uint32_t bits = 0U;
    if (!reader.readBits(3U, bits)) {
      error = "dynamic code-length stream truncated";
      return false;
    }
    codeLengthLengths[static_cast<std::size_t>(kCodeLengthOrder[i])] =
        static_cast<std::uint8_t>(bits);
  }

  DeflateHuffmanTable codeLengthTable{};
  if (!buildDeflateHuffmanTable(codeLengthLengths, codeLengthTable, error)) {
    return false;
  }
  if (codeLengthTable.maxLength == 0U) {
    error = "dynamic code-length table missing";
    return false;
  }

  std::vector<std::uint8_t> allCodeLengths;
  allCodeLengths.reserve(hlit + hdist);

  while (allCodeLengths.size() < hlit + hdist) {
    int symbol = -1;
    if (!decodeDeflateHuffmanSymbol(reader, codeLengthTable, symbol)) {
      error = "failed to decode dynamic code lengths";
      return false;
    }

    if (symbol <= 15) {
      allCodeLengths.push_back(static_cast<std::uint8_t>(symbol));
      continue;
    }

    std::size_t repeatCount = 0U;
    std::uint8_t repeatValue = 0U;

    if (symbol == 16) {
      if (allCodeLengths.empty()) {
        error = "dynamic repeat with no previous length";
        return false;
      }

      std::uint32_t extra = 0U;
      if (!reader.readBits(2U, extra)) {
        error = "dynamic repeat length truncated";
        return false;
      }

      repeatCount = static_cast<std::size_t>(extra) + 3U;
      repeatValue = allCodeLengths.back();
    } else if (symbol == 17) {
      std::uint32_t extra = 0U;
      if (!reader.readBits(3U, extra)) {
        error = "dynamic zero-repeat length truncated";
        return false;
      }

      repeatCount = static_cast<std::size_t>(extra) + 3U;
      repeatValue = 0U;
    } else if (symbol == 18) {
      std::uint32_t extra = 0U;
      if (!reader.readBits(7U, extra)) {
        error = "dynamic long zero-repeat length truncated";
        return false;
      }

      repeatCount = static_cast<std::size_t>(extra) + 11U;
      repeatValue = 0U;
    } else {
      error = "invalid dynamic code-length symbol";
      return false;
    }

    if (allCodeLengths.size() + repeatCount > hlit + hdist) {
      error = "dynamic code lengths overrun expected count";
      return false;
    }

    allCodeLengths.insert(allCodeLengths.end(), repeatCount, repeatValue);
  }

  std::vector<std::uint8_t> litLenLengths(
      allCodeLengths.begin(), allCodeLengths.begin() + static_cast<std::ptrdiff_t>(hlit));
  std::vector<std::uint8_t> distanceLengths(
      allCodeLengths.begin() + static_cast<std::ptrdiff_t>(hlit), allCodeLengths.end());

  if (!buildDeflateHuffmanTable(litLenLengths, litLenTable, error)) {
    return false;
  }

  const bool allDistanceCodesZero = std::all_of(
      distanceLengths.begin(),
      distanceLengths.end(),
      [](std::uint8_t value) {
        return value == 0U;
      });

  if (allDistanceCodesZero) {
    distanceTable = {};
    return true;
  }

  if (!buildDeflateHuffmanTable(distanceLengths, distanceTable, error)) {
    return false;
  }

  return true;
}

}  // namespace volt::io::compression::detail
