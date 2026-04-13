#include "volt/io/compression/HuffmanCodec.hpp"

#include "BitStream.hpp"
#include "CanonicalHuffman.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <queue>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace bitstream = volt::io::compression::detail;
namespace huffman = volt::io::compression::detail;

constexpr std::array<std::uint8_t, 4> kHuffmanMagic{'V', 'H', 'U', 'F'};
constexpr std::uint8_t kHuffmanVersion = 1U;
constexpr std::uint8_t kFlagChecksumPresent = 0x01U;
constexpr std::uint8_t kFlagCodeLengthRle = 0x02U;
constexpr std::size_t kSymbolDomainSize = 256U;
constexpr std::uint8_t kMaxCodeLength = 16U;
constexpr std::uint8_t kFastDecodeBits = 10U;

using BitWriter = bitstream::BitWriter<bitstream::BitOrder::kMsbFirst>;
using BitReader = bitstream::BitReader<bitstream::BitOrder::kMsbFirst>;

using HuffmanCanonicalTable = huffman::CanonicalHuffmanTable;

void appendU16Le(std::vector<std::uint8_t>& output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void appendU32Le(std::vector<std::uint8_t>& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void appendU64Le(std::vector<std::uint8_t>& output, std::uint64_t value) {
  output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 32U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 40U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 48U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 56U) & 0xFFU));
}

bool readU16Le(const std::vector<std::uint8_t>& input, std::size_t& offset, std::uint16_t& value) {
  if (offset + 2U > input.size()) {
    return false;
  }

  value = static_cast<std::uint16_t>(input[offset]) |
          (static_cast<std::uint16_t>(input[offset + 1U]) << 8U);
  offset += 2U;
  return true;
}

bool readU32Le(const std::vector<std::uint8_t>& input, std::size_t& offset, std::uint32_t& value) {
  if (offset + 4U > input.size()) {
    return false;
  }

  value = static_cast<std::uint32_t>(input[offset]) |
          (static_cast<std::uint32_t>(input[offset + 1U]) << 8U) |
          (static_cast<std::uint32_t>(input[offset + 2U]) << 16U) |
          (static_cast<std::uint32_t>(input[offset + 3U]) << 24U);
  offset += 4U;
  return true;
}

bool readU64Le(const std::vector<std::uint8_t>& input, std::size_t& offset, std::uint64_t& value) {
  if (offset + 8U > input.size()) {
    return false;
  }

  value = static_cast<std::uint64_t>(input[offset]) |
          (static_cast<std::uint64_t>(input[offset + 1U]) << 8U) |
          (static_cast<std::uint64_t>(input[offset + 2U]) << 16U) |
          (static_cast<std::uint64_t>(input[offset + 3U]) << 24U) |
          (static_cast<std::uint64_t>(input[offset + 4U]) << 32U) |
          (static_cast<std::uint64_t>(input[offset + 5U]) << 40U) |
          (static_cast<std::uint64_t>(input[offset + 6U]) << 48U) |
          (static_cast<std::uint64_t>(input[offset + 7U]) << 56U);
  offset += 8U;
  return true;
}

void encodeCodeLengthsRle(const std::array<std::uint8_t, kSymbolDomainSize>& codeLengths,
                          std::vector<std::uint8_t>& encoded) {
  encoded.clear();

  std::size_t index = 0U;
  while (index < kSymbolDomainSize) {
    std::size_t repeatLen = 1U;
    while (index + repeatLen < kSymbolDomainSize &&
           codeLengths[index + repeatLen] == codeLengths[index] &&
           repeatLen < 130U) {
      ++repeatLen;
    }

    if (repeatLen >= 4U) {
      encoded.push_back(static_cast<std::uint8_t>(0x80U | static_cast<std::uint8_t>(repeatLen - 3U)));
      encoded.push_back(codeLengths[index]);
      index += repeatLen;
      continue;
    }

    const std::size_t runStart = index;
    std::size_t literalLen = 0U;
    while (index < kSymbolDomainSize && literalLen < 128U) {
      repeatLen = 1U;
      while (index + repeatLen < kSymbolDomainSize &&
             codeLengths[index + repeatLen] == codeLengths[index] &&
             repeatLen < 130U) {
        ++repeatLen;
      }

      if (repeatLen >= 4U && literalLen > 0U) {
        break;
      }

      ++index;
      ++literalLen;
      if (repeatLen >= 4U) {
        break;
      }
    }

    encoded.push_back(static_cast<std::uint8_t>(literalLen - 1U));
    for (std::size_t i = 0U; i < literalLen; ++i) {
      encoded.push_back(codeLengths[runStart + i]);
    }
  }
}

bool decodeCodeLengthsRle(const std::vector<std::uint8_t>& input,
                          std::size_t& offset,
                          std::uint16_t encodedByteCount,
                          std::array<std::uint8_t, kSymbolDomainSize>& codeLengths,
                          std::string& error) {
  if (offset + encodedByteCount > input.size()) {
    error = "truncated huffman code-length rle stream";
    return false;
  }

  const std::size_t endOffset = offset + encodedByteCount;
  std::size_t codeLengthIndex = 0U;
  codeLengths.fill(0U);

  while (offset < endOffset) {
    const std::uint8_t tag = input[offset++];
    if ((tag & 0x80U) != 0U) {
      const std::size_t runLength = static_cast<std::size_t>(tag & 0x7FU) + 3U;
      if (offset >= endOffset) {
        error = "truncated huffman rle repeat value";
        return false;
      }

      const std::uint8_t repeatedValue = input[offset++];
      if (codeLengthIndex + runLength > kSymbolDomainSize) {
        error = "huffman rle repeat overruns code-length table";
        return false;
      }

      for (std::size_t i = 0U; i < runLength; ++i) {
        codeLengths[codeLengthIndex + i] = repeatedValue;
      }
      codeLengthIndex += runLength;
      continue;
    }

    const std::size_t runLength = static_cast<std::size_t>(tag) + 1U;
    if (offset + runLength > endOffset) {
      error = "truncated huffman rle literal run";
      return false;
    }
    if (codeLengthIndex + runLength > kSymbolDomainSize) {
      error = "huffman rle literal overruns code-length table";
      return false;
    }

    for (std::size_t i = 0U; i < runLength; ++i) {
      codeLengths[codeLengthIndex + i] = input[offset + i];
    }
    offset += runLength;
    codeLengthIndex += runLength;
  }

  if (offset != endOffset) {
    error = "huffman rle stream boundary mismatch";
    return false;
  }
  if (codeLengthIndex != kSymbolDomainSize) {
    error = "huffman rle stream did not fill code-length table";
    return false;
  }

  return true;
}

[[nodiscard]] std::uint32_t fnv1a32(const std::uint8_t* data, std::size_t size) {
  constexpr std::uint32_t kFnvOffset = 2166136261U;
  constexpr std::uint32_t kFnvPrime = 16777619U;

  std::uint32_t hash = kFnvOffset;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<std::uint32_t>(data[i]);
    hash *= kFnvPrime;
  }

  return hash;
}

bool buildLengthLimitedCodeLengths(
    const std::array<std::uint64_t, kSymbolDomainSize>& frequencies,
    std::array<std::uint8_t, kSymbolDomainSize>& codeLengths,
    std::string& error) {
  codeLengths.fill(0U);

  std::vector<std::uint8_t> activeSymbols;
  activeSymbols.reserve(kSymbolDomainSize);
  for (std::size_t symbol = 0U; symbol < kSymbolDomainSize; ++symbol) {
    if (frequencies[symbol] != 0U) {
      activeSymbols.push_back(static_cast<std::uint8_t>(symbol));
    }
  }

  if (activeSymbols.empty()) {
    return true;
  }

  if (activeSymbols.size() == 1U) {
    codeLengths[activeSymbols.front()] = 1U;
    return true;
  }

  struct TreeNode {
    std::uint64_t frequency{0U};
    int symbolMin{-1};
    int left{-1};
    int right{-1};
  };

  struct HeapEntry {
    std::uint64_t frequency{0U};
    int symbolMin{-1};
    int nodeIndex{-1};
  };

  struct HeapEntryCompare {
    bool operator()(const HeapEntry& lhs, const HeapEntry& rhs) const {
      if (lhs.frequency != rhs.frequency) {
        return lhs.frequency > rhs.frequency;
      }
      if (lhs.symbolMin != rhs.symbolMin) {
        return lhs.symbolMin > rhs.symbolMin;
      }
      return lhs.nodeIndex > rhs.nodeIndex;
    }
  };

  std::vector<TreeNode> nodes;
  nodes.reserve(activeSymbols.size() * 2U);

  std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapEntryCompare> minHeap;

  for (std::uint8_t symbol : activeSymbols) {
    TreeNode leaf{};
    leaf.frequency = frequencies[symbol];
    leaf.symbolMin = static_cast<int>(symbol);

    const int nodeIndex = static_cast<int>(nodes.size());
    nodes.push_back(leaf);
    minHeap.push({leaf.frequency, leaf.symbolMin, nodeIndex});
  }

  while (minHeap.size() > 1U) {
    const HeapEntry left = minHeap.top();
    minHeap.pop();

    const HeapEntry right = minHeap.top();
    minHeap.pop();

    TreeNode parent{};
    parent.frequency = left.frequency + right.frequency;
    parent.symbolMin = std::min(left.symbolMin, right.symbolMin);
    parent.left = left.nodeIndex;
    parent.right = right.nodeIndex;

    const int parentIndex = static_cast<int>(nodes.size());
    nodes.push_back(parent);
    minHeap.push({parent.frequency, parent.symbolMin, parentIndex});
  }

  const int rootIndex = minHeap.top().nodeIndex;

  std::vector<std::pair<int, int>> nodeStack;
  nodeStack.reserve(nodes.size());
  nodeStack.push_back({rootIndex, 0});

  while (!nodeStack.empty()) {
    const std::pair<int, int> current = nodeStack.back();
    nodeStack.pop_back();

    const TreeNode& node = nodes[static_cast<std::size_t>(current.first)];
    if (node.left < 0 && node.right < 0) {
      const int depth = std::max(1, current.second);
      if (depth > 255) {
        error = "huffman tree depth overflow";
        return false;
      }

      codeLengths[static_cast<std::size_t>(node.symbolMin)] = static_cast<std::uint8_t>(depth);
      continue;
    }

    if (node.right >= 0) {
      nodeStack.push_back({node.right, current.second + 1});
    }
    if (node.left >= 0) {
      nodeStack.push_back({node.left, current.second + 1});
    }
  }

  std::sort(
      activeSymbols.begin(),
      activeSymbols.end(),
      [&frequencies](std::uint8_t lhs, std::uint8_t rhs) {
        const std::uint64_t lhsFreq = frequencies[lhs];
        const std::uint64_t rhsFreq = frequencies[rhs];
        if (lhsFreq != rhsFreq) {
          return lhsFreq < rhsFreq;
        }
        return lhs < rhs;
      });

  std::array<int, kMaxCodeLength + 1U> bitLengthCounts{};
  int overflow = 0;

  for (std::uint8_t symbol : activeSymbols) {
    int length = static_cast<int>(codeLengths[symbol]);
    if (length > static_cast<int>(kMaxCodeLength)) {
      length = static_cast<int>(kMaxCodeLength);
      ++overflow;
    }

    codeLengths[symbol] = static_cast<std::uint8_t>(length);
    ++bitLengthCounts[static_cast<std::size_t>(length)];
  }

  while (overflow > 0) {
    int bits = static_cast<int>(kMaxCodeLength) - 1;
    while (bits > 0 && bitLengthCounts[static_cast<std::size_t>(bits)] == 0) {
      --bits;
    }

    if (bits == 0) {
      error = "unable to enforce max code length";
      return false;
    }

    --bitLengthCounts[static_cast<std::size_t>(bits)];
    bitLengthCounts[static_cast<std::size_t>(bits + 1)] += 2;

    if (bitLengthCounts[kMaxCodeLength] == 0) {
      error = "invalid huffman bit length redistribution";
      return false;
    }

    --bitLengthCounts[kMaxCodeLength];
    overflow -= 2;
  }

  std::array<std::uint8_t, kSymbolDomainSize> reassignedLengths{};
  std::size_t cursor = 0U;

  for (int bits = static_cast<int>(kMaxCodeLength); bits >= 1; --bits) {
    const int count = bitLengthCounts[static_cast<std::size_t>(bits)];
    for (int i = 0; i < count; ++i) {
      if (cursor >= activeSymbols.size()) {
        error = "bit-length redistribution overrun";
        return false;
      }

      reassignedLengths[activeSymbols[cursor]] = static_cast<std::uint8_t>(bits);
      ++cursor;
    }
  }

  if (cursor != activeSymbols.size()) {
    error = "bit-length redistribution underrun";
    return false;
  }

  codeLengths = reassignedLengths;
  return true;
}

bool buildCanonicalTable(
    const std::array<std::uint8_t, kSymbolDomainSize>& codeLengths,
    bool requireSymbols,
    HuffmanCanonicalTable& table,
    std::string& error) {
  std::vector<std::uint8_t> lengths(codeLengths.begin(), codeLengths.end());
  return huffman::buildCanonicalHuffmanTable(
      lengths,
      kMaxCodeLength,
      requireSymbols,
      huffman::HuffmanBitOrder::kMsbFirst,
      kFastDecodeBits,
      table,
      error);
}

bool decodeSymbol(
    BitReader& reader,
    const HuffmanCanonicalTable& table,
    std::uint8_t& symbol) {
  std::uint16_t decoded = 0U;
  if (!huffman::decodeCanonicalHuffmanSymbol(reader, table, decoded)) {
    return false;
  }

  if (decoded > static_cast<std::uint16_t>(std::numeric_limits<std::uint8_t>::max())) {
    return false;
  }

  symbol = static_cast<std::uint8_t>(decoded);
  return true;
}

[[nodiscard]] std::uint16_t countNonZeroCodeLengths(const std::array<std::uint8_t, kSymbolDomainSize>& codeLengths) {
  std::uint16_t count = 0U;
  for (std::uint8_t length : codeLengths) {
    if (length != 0U) {
      ++count;
    }
  }
  return count;
}

}  // namespace

namespace volt::io {

HuffmanCodecResult compressHuffmanData(
    const std::vector<std::uint8_t>& inputData,
    std::vector<std::uint8_t>& compressedData,
    const HuffmanEncodeOptions& options) {
  compressedData.clear();

  std::array<std::uint64_t, kSymbolDomainSize> frequencies{};
  for (std::uint8_t value : inputData) {
    ++frequencies[value];
  }

  std::array<std::uint8_t, kSymbolDomainSize> codeLengths{};
  std::string error;
  if (!buildLengthLimitedCodeLengths(frequencies, codeLengths, error)) {
    return {false, error};
  }

  HuffmanCanonicalTable canonical{};
  if (!buildCanonicalTable(codeLengths, false, canonical, error)) {
    return {false, error};
  }

  const std::uint16_t symbolCount = countNonZeroCodeLengths(codeLengths);
  const std::size_t pairTableSize = static_cast<std::size_t>(symbolCount) * 2U;
  std::vector<std::uint8_t> codeLengthRle;
  encodeCodeLengthsRle(codeLengths, codeLengthRle);

  const bool canUseRle =
      options.preferCodeLengthRle &&
      codeLengthRle.size() <= static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max());
  const std::size_t rleTableSize = canUseRle ? (2U + codeLengthRle.size()) : std::numeric_limits<std::size_t>::max();
  const bool useCodeLengthRle = canUseRle && rleTableSize < pairTableSize;

  std::uint8_t flags = options.includeChecksum ? kFlagChecksumPresent : 0U;
  if (useCodeLengthRle) {
    flags = static_cast<std::uint8_t>(flags | kFlagCodeLengthRle);
  }

  const std::uint32_t checksum = fnv1a32(inputData.data(), inputData.size());

  compressedData.reserve(
      4U + 1U + 1U + 2U + 8U +
      (options.includeChecksum ? 4U : 0U) +
      (useCodeLengthRle ? rleTableSize : pairTableSize) +
      inputData.size());

  compressedData.insert(compressedData.end(), kHuffmanMagic.begin(), kHuffmanMagic.end());
  compressedData.push_back(kHuffmanVersion);
  compressedData.push_back(flags);
  appendU16Le(compressedData, symbolCount);
  appendU64Le(compressedData, static_cast<std::uint64_t>(inputData.size()));

  if (options.includeChecksum) {
    appendU32Le(compressedData, checksum);
  }

  if (useCodeLengthRle) {
    appendU16Le(compressedData, static_cast<std::uint16_t>(codeLengthRle.size()));
    compressedData.insert(compressedData.end(), codeLengthRle.begin(), codeLengthRle.end());
  } else {
    for (std::size_t symbol = 0U; symbol < kSymbolDomainSize; ++symbol) {
      const std::uint8_t length = codeLengths[symbol];
      if (length == 0U) {
        continue;
      }

      compressedData.push_back(static_cast<std::uint8_t>(symbol));
      compressedData.push_back(length);
    }
  }

  BitWriter writer;
  for (std::uint8_t value : inputData) {
    const std::uint8_t length = canonical.codeLengths[value];
    if (length == 0U) {
      return {false, "missing code for input symbol"};
    }
    writer.writeBits(canonical.codes[value], length);
  }
  writer.flush();

  const std::vector<std::uint8_t>& payload = writer.bytes();
  compressedData.insert(compressedData.end(), payload.begin(), payload.end());

  return {true, {}};
}

HuffmanCodecResult decompressHuffmanData(
    const std::vector<std::uint8_t>& compressedData,
    std::vector<std::uint8_t>& decompressedData) {
  decompressedData.clear();

  if (compressedData.size() < 4U + 1U + 1U + 2U + 8U) {
    return {false, "huffman payload too small"};
  }

  std::size_t offset = 0U;
  for (std::uint8_t expected : kHuffmanMagic) {
    if (compressedData[offset] != expected) {
      return {false, "invalid huffman magic"};
    }
    ++offset;
  }

  const std::uint8_t version = compressedData[offset++];
  if (version != kHuffmanVersion) {
    return {false, "unsupported huffman version"};
  }

  const std::uint8_t flags = compressedData[offset++];
  const std::uint8_t supportedFlags = static_cast<std::uint8_t>(kFlagChecksumPresent | kFlagCodeLengthRle);
  if ((flags & static_cast<std::uint8_t>(~supportedFlags)) != 0U) {
    return {false, "unsupported huffman flags"};
  }

  std::uint16_t symbolCount = 0U;
  if (!readU16Le(compressedData, offset, symbolCount)) {
    return {false, "truncated huffman symbol count"};
  }
  if (symbolCount > kSymbolDomainSize) {
    return {false, "invalid huffman symbol count"};
  }

  std::uint64_t originalSize = 0U;
  if (!readU64Le(compressedData, offset, originalSize)) {
    return {false, "truncated huffman original size"};
  }

  std::uint32_t expectedChecksum = 0U;
  const bool checksumPresent = (flags & kFlagChecksumPresent) != 0U;
  if (checksumPresent && !readU32Le(compressedData, offset, expectedChecksum)) {
    return {false, "truncated huffman checksum"};
  }

  std::array<std::uint8_t, kSymbolDomainSize> codeLengths{};
  const bool codeLengthRlePresent = (flags & kFlagCodeLengthRle) != 0U;
  if (codeLengthRlePresent) {
    std::uint16_t encodedByteCount = 0U;
    if (!readU16Le(compressedData, offset, encodedByteCount)) {
      return {false, "truncated huffman code-length rle size"};
    }

    std::string rleError;
    if (!decodeCodeLengthsRle(compressedData, offset, encodedByteCount, codeLengths, rleError)) {
      return {false, rleError};
    }
  } else {
    for (std::uint16_t i = 0U; i < symbolCount; ++i) {
      if (offset + 2U > compressedData.size()) {
        return {false, "truncated huffman code-length table"};
      }

      const std::uint8_t symbol = compressedData[offset++];
      const std::uint8_t length = compressedData[offset++];

      if (length == 0U || length > kMaxCodeLength) {
        return {false, "invalid huffman code length entry"};
      }
      if (codeLengths[symbol] != 0U) {
        return {false, "duplicate huffman symbol entry"};
      }

      codeLengths[symbol] = length;
    }
  }

  for (std::uint8_t length : codeLengths) {
    if (length > kMaxCodeLength) {
      return {false, "invalid huffman code length entry"};
    }
  }

  if (countNonZeroCodeLengths(codeLengths) != symbolCount) {
    return {false, "huffman symbol table mismatch"};
  }

  if (originalSize == 0U) {
    if (symbolCount != 0U) {
      return {false, "empty payload must not declare symbols"};
    }

    if (checksumPresent) {
      const std::uint32_t checksum = fnv1a32(nullptr, 0U);
      if (checksum != expectedChecksum) {
        return {false, "huffman checksum mismatch"};
      }
    }

    if (offset != compressedData.size()) {
      for (std::size_t i = offset; i < compressedData.size(); ++i) {
        if (compressedData[i] != 0U) {
          return {false, "non-zero trailing bytes after empty payload"};
        }
      }
    }

    return {true, {}};
  }

  if (symbolCount == 0U) {
    return {false, "non-empty payload missing symbols"};
  }

  std::string error;
  HuffmanCanonicalTable canonical{};
  if (!buildCanonicalTable(codeLengths, true, canonical, error)) {
    return {false, error};
  }

  std::vector<std::uint8_t> payload(
      compressedData.begin() + static_cast<std::ptrdiff_t>(offset),
      compressedData.end());

  BitReader reader(payload);
  if (originalSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return {false, "huffman payload too large for platform"};
  }

  decompressedData.reserve(static_cast<std::size_t>(originalSize));

  while (decompressedData.size() < originalSize) {
    std::uint8_t symbol = 0U;
    if (!decodeSymbol(reader, canonical, symbol)) {
      decompressedData.clear();
      return {false, "huffman payload bitstream decode failed"};
    }

    decompressedData.push_back(symbol);
  }

  if (!reader.remainingBitsAreZero()) {
    decompressedData.clear();
    return {false, "non-zero trailing huffman padding bits"};
  }

  if (checksumPresent) {
    const std::uint32_t actualChecksum = fnv1a32(decompressedData.data(), decompressedData.size());
    if (actualChecksum != expectedChecksum) {
      decompressedData.clear();
      return {false, "huffman checksum mismatch"};
    }
  }

  return {true, {}};
}

}  // namespace volt::io
