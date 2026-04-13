#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace volt::io::compression::detail {

enum class HuffmanBitOrder : std::uint8_t {
  kMsbFirst,
  kLsbFirst,
};

struct HuffmanFastDecodeEntry {
  std::uint16_t symbol{0U};
  std::uint8_t bitLength{0U};
};

struct CanonicalHuffmanTable {
  std::vector<std::uint8_t> codeLengths;
  std::vector<std::uint16_t> codes;
  std::vector<std::uint16_t> codeCounts;
  std::vector<std::vector<int>> decodeByLength;
  std::vector<HuffmanFastDecodeEntry> fastDecode;
  std::uint8_t maxLength{0U};
  std::uint8_t maxBitLength{0U};
  std::uint8_t fastDecodeBits{0U};
  HuffmanBitOrder bitOrder{HuffmanBitOrder::kMsbFirst};
};

[[nodiscard]] std::uint16_t reverseBits16(std::uint16_t value, std::uint8_t bits);

bool buildCanonicalHuffmanTable(const std::vector<std::uint8_t>& codeLengths,
                                std::uint8_t maxBitLength,
                                bool requireSymbols,
                                HuffmanBitOrder bitOrder,
                                std::uint8_t fastDecodeBits,
                                CanonicalHuffmanTable& table,
                                std::string& error);

template <typename TBitReader>
bool decodeCanonicalHuffmanSymbol(TBitReader& reader,
                                  const CanonicalHuffmanTable& table,
                                  std::uint16_t& symbol) {
  symbol = 0U;

  if (table.maxLength == 0U) {
    return false;
  }

  if (table.fastDecodeBits > 0U && !table.fastDecode.empty()) {
    std::uint32_t prefix = 0U;
    if (reader.peekBits(table.fastDecodeBits, prefix)) {
      if (prefix < table.fastDecode.size()) {
        const HuffmanFastDecodeEntry entry = table.fastDecode[prefix];
        if (entry.bitLength != 0U) {
          if (!reader.dropBits(entry.bitLength)) {
            return false;
          }

          symbol = entry.symbol;
          return true;
        }
      }
    }
  }

  std::uint32_t code = 0U;
  for (std::uint8_t length = 1U; length <= table.maxLength; ++length) {
    std::uint32_t bit = 0U;
    if (!reader.readBits(1U, bit)) {
      return false;
    }

    if (table.bitOrder == HuffmanBitOrder::kMsbFirst) {
      code = (code << 1U) | bit;
    } else {
      code |= bit << static_cast<unsigned int>(length - 1U);
    }

    if (length >= table.decodeByLength.size()) {
      continue;
    }

    const std::vector<int>& bucket = table.decodeByLength[length];
    if (bucket.empty() || code >= bucket.size()) {
      continue;
    }

    const int decoded = bucket[code];
    if (decoded < 0) {
      continue;
    }

    symbol = static_cast<std::uint16_t>(decoded);
    return true;
  }

  return false;
}

}  // namespace volt::io::compression::detail
