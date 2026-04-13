#include "volt/io/compression/DeflateCodec.hpp"

#include "DeflateHuffman.hpp"
#include "ZlibEnvelope.hpp"

#include "volt/core/Logging.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace deflate_huffman = volt::io::compression::detail;

constexpr std::size_t kMaxInflateOutputBytes = 256U * 1024U * 1024U;
constexpr std::size_t kDeflateWindowSize = 32768U;
constexpr std::size_t kDeflateMaxMatchLength = 258U;
constexpr std::size_t kDeflateMinMatchLength = 3U;
constexpr std::size_t kMatchHashBits = 15U;
constexpr std::size_t kMatchHashSize = 1U << kMatchHashBits;

constexpr std::array<int, 29> kLengthBase = {
    3,    4,    5,    6,    7,    8,     9,     10,    11,    13,
    15,   17,   19,   23,   27,   31,    35,    43,    51,    59,
    67,   83,   99,   115,  131,  163,   195,   227,   258,
};

constexpr std::array<int, 29> kLengthExtraBits = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
    1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0,
};

constexpr std::array<int, 30> kDistanceBase = {
    1,     2,     3,     4,     5,     7,     9,     13,    17,    25,
    33,    49,    65,    97,    129,   193,   257,   385,   513,   769,
    1025,  1537,  2049,  3073,  4097,  6145,  8193,  12289, 16385, 24577,
};

constexpr std::array<int, 30> kDistanceExtraBits = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3,
    4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

std::uint32_t adler32(const std::uint8_t* data, std::size_t size) {
  constexpr std::uint32_t kMod = 65521U;
  constexpr std::size_t kNMax = 5552U;
  std::uint32_t a = 1U;
  std::uint32_t b = 0U;

  std::size_t offset = 0U;
  while (offset < size) {
    const std::size_t block = std::min(kNMax, size - offset);
    for (std::size_t i = 0U; i < block; ++i) {
      a += data[offset + i];
      b += a;
    }

    a %= kMod;
    b %= kMod;
    offset += block;
  }

  return (b << 16U) | a;
}

using BitReader = deflate_huffman::DeflateBitReader;
using BitWriter = deflate_huffman::BitWriter<deflate_huffman::BitOrder::kLsbFirst>;
using HuffmanTable = deflate_huffman::DeflateHuffmanTable;
using ZlibEnvelopeView = deflate_huffman::ZlibEnvelopeView;

std::uint32_t hashMatch3(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
  const std::uint32_t packed =
      (static_cast<std::uint32_t>(a) << 16U) |
      (static_cast<std::uint32_t>(b) << 8U) |
      static_cast<std::uint32_t>(c);
  return (packed * 2654435761U) >> (32U - static_cast<std::uint32_t>(kMatchHashBits));
}

int maxChainFromQuality(int quality) {
  static constexpr std::array<int, 10> kChainByQuality = {
      0,
      4,
      8,
      16,
      24,
      32,
      48,
      64,
      96,
      128,
  };
  return kChainByQuality[static_cast<std::size_t>(std::clamp(quality, 0, 9))];
}

std::size_t niceMatchFromQuality(int quality) {
  static constexpr std::array<std::size_t, 10> kNiceByQuality = {
      0U,
      16U,
      32U,
      64U,
      96U,
      128U,
      160U,
      192U,
      224U,
      258U,
  };
  return kNiceByQuality[static_cast<std::size_t>(std::clamp(quality, 0, 9))];
}

bool writeDeflateHuffmanSymbol(BitWriter& writer,
                               const HuffmanTable& table,
                               std::size_t symbol,
                               std::string& error) {
  if (symbol >= table.codeLengths.size()) {
    error = "deflate symbol out of range";
    return false;
  }

  const std::uint8_t bitLength = table.codeLengths[symbol];
  if (bitLength == 0U) {
    error = "deflate symbol has no code";
    return false;
  }

  writer.writeBits(table.codes[symbol], bitLength);
  return true;
}

bool encodeLengthCode(std::size_t length,
                      std::size_t& symbol,
                      std::uint8_t& extraBits,
                      std::uint32_t& extraValue,
                      std::string& error) {
  if (length < kDeflateMinMatchLength || length > kDeflateMaxMatchLength) {
    error = "deflate length out of range";
    return false;
  }

  for (std::size_t i = 0U; i < kLengthBase.size(); ++i) {
    const std::size_t base = static_cast<std::size_t>(kLengthBase[i]);
    const std::size_t bits = static_cast<std::size_t>(kLengthExtraBits[i]);
    const std::size_t max = base + ((static_cast<std::size_t>(1U) << bits) - 1U);
    if (length <= max) {
      symbol = 257U + i;
      extraBits = static_cast<std::uint8_t>(bits);
      extraValue = static_cast<std::uint32_t>(length - base);
      return true;
    }
  }

  error = "deflate length symbol lookup failed";
  return false;
}

bool encodeDistanceCode(std::size_t distance,
                        std::size_t& symbol,
                        std::uint8_t& extraBits,
                        std::uint32_t& extraValue,
                        std::string& error) {
  if (distance == 0U || distance > kDeflateWindowSize) {
    error = "deflate distance out of range";
    return false;
  }

  for (std::size_t i = 0U; i < kDistanceBase.size(); ++i) {
    const std::size_t base = static_cast<std::size_t>(kDistanceBase[i]);
    const std::size_t bits = static_cast<std::size_t>(kDistanceExtraBits[i]);
    const std::size_t max = base + ((static_cast<std::size_t>(1U) << bits) - 1U);
    if (distance <= max) {
      symbol = i;
      extraBits = static_cast<std::uint8_t>(bits);
      extraValue = static_cast<std::uint32_t>(distance - base);
      return true;
    }
  }

  error = "deflate distance symbol lookup failed";
  return false;
}

void insertMatchPosition(const std::vector<std::uint8_t>& inputData,
                         std::vector<int>& head,
                         std::vector<int>& prev,
                         std::size_t pos) {
  if (pos + 2U >= inputData.size()) {
    return;
  }

  const std::size_t hash = static_cast<std::size_t>(hashMatch3(
      inputData[pos + 0U],
      inputData[pos + 1U],
      inputData[pos + 2U]));
  prev[pos] = head[hash];
  head[hash] = static_cast<int>(pos);
}

bool encodeFixedHuffmanRawDeflate(const std::vector<std::uint8_t>& inputData,
                                  std::vector<std::uint8_t>& outputData,
                                  int quality,
                                  std::string& error) {
  outputData.clear();
  if (inputData.empty()) {
    return true;
  }

  HuffmanTable litLenTable{};
  HuffmanTable distanceTable{};
  if (!deflate_huffman::buildFixedDeflateHuffmanTables(litLenTable, distanceTable, error)) {
    return false;
  }

  std::vector<int> head(kMatchHashSize, -1);
  std::vector<int> prev(inputData.size(), -1);

  const int maxChain = maxChainFromQuality(quality);
  const std::size_t niceMatch = niceMatchFromQuality(quality);

  BitWriter writer;
  writer.writeBits(1U, 1U);  // BFINAL=1
  writer.writeBits(1U, 2U);  // BTYPE=01 (fixed huffman)

  std::size_t pos = 0U;
  while (pos < inputData.size()) {
    std::size_t bestLength = 0U;
    std::size_t bestDistance = 0U;

    if (maxChain > 0 && pos + 2U < inputData.size()) {
      const std::size_t hash = static_cast<std::size_t>(hashMatch3(
          inputData[pos + 0U],
          inputData[pos + 1U],
          inputData[pos + 2U]));
      int candidate = head[hash];
      int remainingChain = maxChain;

      while (candidate >= 0 && remainingChain-- > 0) {
        const std::size_t candidatePos = static_cast<std::size_t>(candidate);
        const std::size_t distance = pos - candidatePos;
        if (distance > kDeflateWindowSize) {
          break;
        }

        if (inputData[candidatePos] != inputData[pos] ||
            inputData[candidatePos + 1U] != inputData[pos + 1U] ||
            inputData[candidatePos + 2U] != inputData[pos + 2U]) {
          candidate = prev[candidatePos];
          continue;
        }

        const std::size_t maxLength = std::min(
            kDeflateMaxMatchLength,
            inputData.size() - pos);

        std::size_t length = 3U;
        while (length < maxLength && inputData[candidatePos + length] == inputData[pos + length]) {
          ++length;
        }

        if (length > bestLength) {
          bestLength = length;
          bestDistance = distance;
          if (length >= niceMatch) {
            break;
          }
        }

        candidate = prev[candidatePos];
      }
    }

    if (bestLength >= kDeflateMinMatchLength) {
      std::size_t lengthSymbol = 0U;
      std::uint8_t lengthExtraBits = 0U;
      std::uint32_t lengthExtraValue = 0U;
      if (!encodeLengthCode(bestLength, lengthSymbol, lengthExtraBits, lengthExtraValue, error)) {
        return false;
      }

      std::size_t distanceSymbol = 0U;
      std::uint8_t distanceExtraBits = 0U;
      std::uint32_t distanceExtraValue = 0U;
      if (!encodeDistanceCode(bestDistance, distanceSymbol, distanceExtraBits, distanceExtraValue, error)) {
        return false;
      }

      if (!writeDeflateHuffmanSymbol(writer, litLenTable, lengthSymbol, error)) {
        return false;
      }

      if (lengthExtraBits > 0U) {
        writer.writeBits(lengthExtraValue, lengthExtraBits);
      }

      if (!writeDeflateHuffmanSymbol(writer, distanceTable, distanceSymbol, error)) {
        return false;
      }

      if (distanceExtraBits > 0U) {
        writer.writeBits(distanceExtraValue, distanceExtraBits);
      }

      for (std::size_t i = 0U; i < bestLength; ++i) {
        insertMatchPosition(inputData, head, prev, pos + i);
      }
      pos += bestLength;
      continue;
    }

    if (!writeDeflateHuffmanSymbol(writer, litLenTable, inputData[pos], error)) {
      return false;
    }

    insertMatchPosition(inputData, head, prev, pos);
    ++pos;
  }

  if (!writeDeflateHuffmanSymbol(writer, litLenTable, 256U, error)) {
    return false;
  }

  writer.flush();
  outputData = writer.bytes();
  return true;
}

bool appendDistanceCopy(std::vector<std::uint8_t>& output,
                        std::size_t distance,
                        std::size_t length,
                        std::string& error) {
  if (distance == 0U || distance > output.size()) {
    error = "invalid deflate distance";
    return false;
  }

  if (length > kMaxInflateOutputBytes || output.size() > kMaxInflateOutputBytes - length) {
    error = "deflate output exceeds safety limit";
    return false;
  }

  const std::size_t oldSize = output.size();
  const std::size_t source = oldSize - distance;
  output.resize(oldSize + length);

  std::uint8_t* out = output.data();
  if (distance == 1U) {
    std::memset(out + oldSize, out[source], length);
    return true;
  }

  for (std::size_t i = 0U; i < length; ++i) {
    out[oldSize + i] = out[source + i];
  }

  return true;
}

bool decodeStoredBlock(BitReader& reader,
                       std::vector<std::uint8_t>& output,
                       std::string& error) {
  reader.alignToByte();

  std::array<std::uint8_t, 4> header{};
  if (!reader.readBytesAligned(header.data(), header.size())) {
    error = "stored deflate block truncated";
    return false;
  }

  const std::uint16_t len = static_cast<std::uint16_t>(header[0]) |
                            (static_cast<std::uint16_t>(header[1]) << 8U);
  const std::uint16_t nlen = static_cast<std::uint16_t>(header[2]) |
                             (static_cast<std::uint16_t>(header[3]) << 8U);
  if (nlen != static_cast<std::uint16_t>(~len)) {
    error = "stored deflate LEN checksum mismatch";
    return false;
  }

  if (output.size() > kMaxInflateOutputBytes - len) {
    error = "stored deflate output exceeds safety limit";
    return false;
  }

  const std::size_t priorSize = output.size();
  output.resize(priorSize + len);
  if (!reader.readBytesAligned(output.data() + priorSize, len)) {
    error = "stored deflate payload truncated";
    return false;
  }

  return true;
}

bool decodeHuffmanBlock(BitReader& reader,
                        const HuffmanTable& litLenTable,
                        const HuffmanTable& distanceTable,
                        std::vector<std::uint8_t>& output,
                        std::string& error) {
  while (true) {
    int symbol = -1;
    if (!deflate_huffman::decodeDeflateHuffmanSymbol(reader, litLenTable, symbol)) {
      error = "deflate literal/length decode failed";
      return false;
    }

    if (symbol < 256) {
      if (output.size() >= kMaxInflateOutputBytes) {
        error = "deflate output exceeds safety limit";
        return false;
      }

      output.push_back(static_cast<std::uint8_t>(symbol));
      continue;
    }

    if (symbol == 256) {
      return true;
    }

    if (symbol < 257 || symbol > 285) {
      error = "deflate literal/length symbol out of range";
      return false;
    }

    const std::size_t lengthIndex = static_cast<std::size_t>(symbol - 257);
    std::size_t length = static_cast<std::size_t>(kLengthBase[lengthIndex]);

    const int lengthExtraBits = kLengthExtraBits[lengthIndex];
    if (lengthExtraBits > 0) {
      std::uint32_t extra = 0U;
      if (!reader.readBits(static_cast<std::uint8_t>(lengthExtraBits), extra)) {
        error = "deflate length extra bits truncated";
        return false;
      }
      length += extra;
    }

    if (distanceTable.maxLength == 0U) {
      error = "deflate distance table missing";
      return false;
    }

    int distanceSymbol = -1;
    if (!deflate_huffman::decodeDeflateHuffmanSymbol(reader, distanceTable, distanceSymbol)) {
      error = "deflate distance decode failed";
      return false;
    }

    if (distanceSymbol < 0 || distanceSymbol > 29) {
      error = "deflate distance symbol out of range";
      return false;
    }

    std::size_t distance = static_cast<std::size_t>(kDistanceBase[static_cast<std::size_t>(distanceSymbol)]);
    const int distanceExtraBits = kDistanceExtraBits[static_cast<std::size_t>(distanceSymbol)];
    if (distanceExtraBits > 0) {
      std::uint32_t extra = 0U;
      if (!reader.readBits(static_cast<std::uint8_t>(distanceExtraBits), extra)) {
        error = "deflate distance extra bits truncated";
        return false;
      }
      distance += extra;
    }

    if (!appendDistanceCopy(output, distance, length, error)) {
      return false;
    }
  }
}

bool inflateRawDeflate(const std::vector<std::uint8_t>& compressedData,
                      std::size_t payloadOffset,
                      std::size_t payloadSize,
                      std::vector<std::uint8_t>& decompressedData,
                      std::string& error) {
  BitReader reader(compressedData, payloadOffset, payloadSize);
  decompressedData.clear();

  bool finalBlock = false;
  while (!finalBlock) {
    std::uint32_t finalBit = 0U;
    std::uint32_t blockType = 0U;
    if (!reader.readBits(1U, finalBit) || !reader.readBits(2U, blockType)) {
      error = "deflate block header truncated";
      return false;
    }
    finalBlock = finalBit != 0U;

    if (blockType == 0U) {
      if (!decodeStoredBlock(reader, decompressedData, error)) {
        return false;
      }
      continue;
    }

    HuffmanTable litLenTable{};
    HuffmanTable distanceTable{};

    if (blockType == 1U) {
      if (!deflate_huffman::buildFixedDeflateHuffmanTables(litLenTable, distanceTable, error)) {
        return false;
      }
    } else if (blockType == 2U) {
      if (!deflate_huffman::buildDynamicDeflateHuffmanTables(reader, litLenTable, distanceTable, error)) {
        return false;
      }
    } else {
      error = "deflate block type reserved";
      return false;
    }

    if (!decodeHuffmanBlock(reader, litLenTable, distanceTable, decompressedData, error)) {
      return false;
    }
  }

  return true;
}

// Stored-block-only encoder used by current zlib compressor path.
void encodeStoredRawDeflate(const std::vector<std::uint8_t>& inputData,
                            std::vector<std::uint8_t>& outputData) {
  outputData.clear();
  if (inputData.empty()) {
    return;
  }

  constexpr std::size_t kMaxStoredBlock = 65535U;

  std::size_t offset = 0U;
  while (offset < inputData.size()) {
    const std::size_t remaining = inputData.size() - offset;
    const std::size_t blockSize = std::min(kMaxStoredBlock, remaining);
    const bool isFinal = (offset + blockSize) == inputData.size();

    outputData.push_back(isFinal ? 0x01U : 0x00U);

    const std::uint16_t len = static_cast<std::uint16_t>(blockSize);
    const std::uint16_t nlen = static_cast<std::uint16_t>(~len);

    outputData.push_back(static_cast<std::uint8_t>(len & 0xFFU));
    outputData.push_back(static_cast<std::uint8_t>((len >> 8U) & 0xFFU));
    outputData.push_back(static_cast<std::uint8_t>(nlen & 0xFFU));
    outputData.push_back(static_cast<std::uint8_t>((nlen >> 8U) & 0xFFU));

    outputData.insert(
        outputData.end(),
        inputData.begin() + static_cast<std::ptrdiff_t>(offset),
        inputData.begin() + static_cast<std::ptrdiff_t>(offset + blockSize));

    offset += blockSize;
  }
}

std::uint8_t zlibFlevelFromQuality(int quality) {
  if (quality >= 7) {
    return 3U;
  }
  if (quality >= 4) {
    return 2U;
  }
  if (quality >= 1) {
    return 1U;
  }
  return 0U;
}

}  // namespace

namespace volt::io {

CompressionResult decompressData(const std::vector<std::uint8_t>& compressedData,
                                 std::vector<std::uint8_t>& decompressedData,
                                 CompressionContainer container) {
  decompressedData.clear();

  if (compressedData.empty()) {
    return {true, {}};
  }

  std::size_t payloadOffset = 0U;
  std::size_t payloadSize = compressedData.size();
  std::uint32_t expectedAdler = 0U;

  if (container == CompressionContainer::kZlib) {
    ZlibEnvelopeView zlibEnvelope{};
    std::string zlibError;
    if (!deflate_huffman::parseZlibEnvelope(compressedData, zlibEnvelope, zlibError)) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "zlib envelope parse failed (reason=",
          zlibError,
          ")");
      return {false, zlibError};
    }

    payloadOffset = zlibEnvelope.payloadOffset;
    payloadSize = zlibEnvelope.payloadSize;
    expectedAdler = zlibEnvelope.expectedAdler;
  }

  std::string inflateError;
  if (!inflateRawDeflate(compressedData, payloadOffset, payloadSize, decompressedData, inflateError)) {
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "deflate decode failed (container=",
        container == CompressionContainer::kRawDeflate ? "raw" : "zlib",
        ", reason=",
        inflateError,
        ")");
    decompressedData.clear();
    return {false, "decompression failed"};
  }

  if (container == CompressionContainer::kZlib) {
    const std::uint32_t actualAdler = adler32(decompressedData.data(), decompressedData.size());
    if (actualAdler != expectedAdler) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "zlib adler mismatch (expected=",
          expectedAdler,
          ", actual=",
          actualAdler,
          ")");
      decompressedData.clear();
      return {false, "zlib checksum mismatch"};
    }
  }

  return {true, {}};
}

CompressionResult compressZlibData(const std::vector<std::uint8_t>& inputData,
                                   std::vector<std::uint8_t>& compressedData,
                                   int quality) {
  compressedData.clear();

  if (inputData.empty()) {
    return {true, {}};
  }

  quality = std::clamp(quality, 0, 9);

  std::vector<std::uint8_t> storedRawDeflate;
  storedRawDeflate.reserve(inputData.size() + (inputData.size() / 65535U + 1U) * 5U);
  encodeStoredRawDeflate(inputData, storedRawDeflate);

  std::vector<std::uint8_t> rawDeflate = storedRawDeflate;
  if (quality >= 2) {
    std::vector<std::uint8_t> fixedRawDeflate;
    std::string encodeError;
    if (encodeFixedHuffmanRawDeflate(inputData, fixedRawDeflate, quality, encodeError)) {
      if (!fixedRawDeflate.empty() && fixedRawDeflate.size() < storedRawDeflate.size()) {
        rawDeflate = std::move(fixedRawDeflate);
      }
    }
  }

  const std::uint8_t cmf = 0x78U;
  const std::uint8_t flevel = zlibFlevelFromQuality(quality);

  std::uint8_t flg = static_cast<std::uint8_t>(flevel << 6U);
  const std::uint16_t headerNoCheck = (static_cast<std::uint16_t>(cmf) << 8U) | flg;
  const std::uint8_t fcheck = static_cast<std::uint8_t>((31 - (headerNoCheck % 31U)) % 31U);
  flg = static_cast<std::uint8_t>(flg | fcheck);

  const std::uint32_t checksum = adler32(inputData.data(), inputData.size());

  compressedData.clear();
  compressedData.reserve(2U + rawDeflate.size() + 4U);
  compressedData.push_back(cmf);
  compressedData.push_back(flg);
  compressedData.insert(compressedData.end(), rawDeflate.begin(), rawDeflate.end());
  deflate_huffman::appendU32Be(compressedData, checksum);

  return {true, {}};
}

}  // namespace volt::io
