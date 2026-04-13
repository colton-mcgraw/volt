#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace volt::io {

struct HuffmanCodecResult {
  bool success{false};
  std::string errorMessage;
};

struct HuffmanEncodeOptions {
  bool includeChecksum{true};
  bool preferCodeLengthRle{true};
};

// Wire format (little-endian fields):
// - magic[4] = "VHUF"
// - version[1] = 1
// - flags[1] (bit 0: checksum present, bit 1: code-length RLE table)
// - symbolCount[2] (number of symbol/length entries)
// - originalSize[8] (decoded byte length)
// - optional checksum[4] (FNV-1a over decoded bytes)
// - if RLE flag clear: symbolCount entries of symbol[1], codeLength[1]
// - if RLE flag set: codeLengthRleSize[2], then code-length RLE stream for 256 symbols
// - encoded payload bits (MSB-first)
[[nodiscard]] HuffmanCodecResult compressHuffmanData(
    const std::vector<std::uint8_t>& inputData,
    std::vector<std::uint8_t>& compressedData,
    const HuffmanEncodeOptions& options = {});

[[nodiscard]] HuffmanCodecResult decompressHuffmanData(
    const std::vector<std::uint8_t>& compressedData,
    std::vector<std::uint8_t>& decompressedData);

}  // namespace volt::io
