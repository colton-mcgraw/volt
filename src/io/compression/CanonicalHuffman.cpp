#include "CanonicalHuffman.hpp"

#include <algorithm>

namespace volt::io::compression::detail {

std::uint16_t reverseBits16(std::uint16_t value, std::uint8_t bits) {
  std::uint16_t out = 0U;
  for (std::uint8_t i = 0U; i < bits; ++i) {
    out = static_cast<std::uint16_t>((out << 1U) | (value & 0x01U));
    value = static_cast<std::uint16_t>(value >> 1U);
  }
  return out;
}

bool buildCanonicalHuffmanTable(const std::vector<std::uint8_t>& codeLengths,
                                std::uint8_t maxBitLength,
                                bool requireSymbols,
                                HuffmanBitOrder bitOrder,
                                std::uint8_t fastDecodeBits,
                                CanonicalHuffmanTable& table,
                                std::string& error) {
  table = {};
  table.bitOrder = bitOrder;
  table.maxBitLength = maxBitLength;
  table.fastDecodeBits = fastDecodeBits;
  table.codeLengths = codeLengths;
  table.codes.assign(codeLengths.size(), 0U);
  table.codeCounts.assign(static_cast<std::size_t>(maxBitLength) + 1U, 0U);
  table.decodeByLength.assign(static_cast<std::size_t>(maxBitLength) + 1U, {});

  if (fastDecodeBits > 0U) {
    const std::size_t fastDecodeSize = static_cast<std::size_t>(1U) << fastDecodeBits;
    table.fastDecode.assign(fastDecodeSize, {});
  }

  std::size_t symbolCount = 0U;
  for (std::uint8_t length : codeLengths) {
    if (length > maxBitLength) {
      error = "code length exceeds maximum";
      return false;
    }

    if (length == 0U) {
      continue;
    }

    ++symbolCount;
    ++table.codeCounts[length];
    table.maxLength = std::max(table.maxLength, length);
  }

  if (symbolCount == 0U) {
    if (requireSymbols) {
      error = "missing huffman symbols";
      return false;
    }
    return true;
  }

  int left = 1;
  for (std::size_t bits = 1U; bits <= maxBitLength; ++bits) {
    left <<= 1;
    left -= static_cast<int>(table.codeCounts[bits]);
    if (left < 0) {
      error = "oversubscribed huffman code lengths";
      return false;
    }
  }

  std::vector<std::uint16_t> nextCode(static_cast<std::size_t>(maxBitLength) + 1U, 0U);
  std::uint32_t code = 0U;
  for (std::size_t bits = 1U; bits <= maxBitLength; ++bits) {
    code = (code + table.codeCounts[bits - 1U]) << 1U;
    if (table.codeCounts[bits] > 0U) {
      const std::uint32_t limit = 1U << bits;
      if (code + table.codeCounts[bits] > limit) {
        error = "invalid canonical huffman ranges";
        return false;
      }
    }
    nextCode[bits] = static_cast<std::uint16_t>(code);
  }

  for (std::size_t symbol = 0U; symbol < codeLengths.size(); ++symbol) {
    const std::uint8_t length = codeLengths[symbol];
    if (length == 0U) {
      continue;
    }

    const std::uint16_t canonicalCode = nextCode[length]++;
    std::uint16_t storedCode = canonicalCode;
    if (bitOrder == HuffmanBitOrder::kLsbFirst) {
      storedCode = reverseBits16(canonicalCode, length);
    }

    table.codes[symbol] = storedCode;

    std::vector<int>& bucket = table.decodeByLength[length];
    if (bucket.empty()) {
      bucket.assign(static_cast<std::size_t>(1U) << length, -1);
    }

    if (storedCode >= bucket.size()) {
      error = "huffman code overflow";
      return false;
    }
    if (bucket[storedCode] >= 0) {
      error = "duplicate huffman code";
      return false;
    }

    bucket[storedCode] = static_cast<int>(symbol);

    if (fastDecodeBits > 0U && length <= fastDecodeBits) {
      const std::uint32_t base = static_cast<std::uint32_t>(storedCode)
                                 << static_cast<unsigned int>(fastDecodeBits - length);
      const std::uint32_t span = 1U << static_cast<unsigned int>(fastDecodeBits - length);
      for (std::uint32_t i = 0U; i < span; ++i) {
        const std::size_t index = static_cast<std::size_t>(base + i);
        HuffmanFastDecodeEntry& entry = table.fastDecode[index];
        if (entry.bitLength != 0U &&
            (entry.bitLength != length || entry.symbol != symbol)) {
          error = "conflicting fast huffman decode entries";
          return false;
        }

        entry.symbol = static_cast<std::uint16_t>(symbol);
        entry.bitLength = length;
      }
    }
  }

  return true;
}

}  // namespace volt::io::compression::detail
