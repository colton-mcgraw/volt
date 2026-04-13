#include "volt/io/compression/HuffmanCodec.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::uint8_t kFlagCodeLengthRle = 0x02U;

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[huffman-test] FAIL: " << message << '\n';
    return false;
  }
  return true;
}

void appendU16Le(std::vector<std::uint8_t>& output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
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

std::vector<std::uint8_t> makeSkewedSample() {
  std::vector<std::uint8_t> data;
  data.reserve(16384);

  for (std::size_t i = 0U; i < 12000U; ++i) {
    data.push_back(static_cast<std::uint8_t>('A'));
  }
  for (std::size_t i = 0U; i < 3000U; ++i) {
    data.push_back(static_cast<std::uint8_t>('B'));
  }
  for (std::size_t i = 0U; i < 1200U; ++i) {
    data.push_back(static_cast<std::uint8_t>('C'));
  }
  for (std::size_t i = 0U; i < 184U; ++i) {
    data.push_back(static_cast<std::uint8_t>((i * 37U + 19U) & 0xFFU));
  }

  return data;
}

std::vector<std::uint8_t> makeWideSample() {
  std::vector<std::uint8_t> data;
  data.reserve(256U * 8U);

  for (std::uint32_t round = 0U; round < 8U; ++round) {
    for (std::uint32_t symbol = 0U; symbol < 256U; ++symbol) {
      data.push_back(static_cast<std::uint8_t>((symbol + round * 13U) & 0xFFU));
    }
  }

  return data;
}

std::vector<std::uint8_t> makeDeepTreeSample() {
  std::vector<std::uint8_t> data;

  std::uint32_t prev = 1U;
  std::uint32_t curr = 1U;
  for (std::uint32_t symbol = 0U; symbol < 14U; ++symbol) {
    const std::uint32_t count = symbol < 2U ? 1U : curr;
    for (std::uint32_t i = 0U; i < count; ++i) {
      data.push_back(static_cast<std::uint8_t>(symbol));
    }

    const std::uint32_t next = prev + curr;
    prev = curr;
    curr = next;
  }

  return data;
}

std::vector<std::uint8_t> makeOversubscribedHeader() {
  std::vector<std::uint8_t> payload;
  payload.push_back('V');
  payload.push_back('H');
  payload.push_back('U');
  payload.push_back('F');
  payload.push_back(1U);
  payload.push_back(0U);
  appendU16Le(payload, 3U);
  appendU64Le(payload, 1U);

  payload.push_back(0U);
  payload.push_back(1U);
  payload.push_back(1U);
  payload.push_back(1U);
  payload.push_back(2U);
  payload.push_back(1U);

  payload.push_back(0x00U);
  return payload;
}

std::vector<std::uint8_t> makeDuplicateSymbolHeader() {
  std::vector<std::uint8_t> payload;
  payload.push_back('V');
  payload.push_back('H');
  payload.push_back('U');
  payload.push_back('F');
  payload.push_back(1U);
  payload.push_back(0U);
  appendU16Le(payload, 2U);
  appendU64Le(payload, 1U);

  payload.push_back(7U);
  payload.push_back(1U);
  payload.push_back(7U);
  payload.push_back(2U);

  payload.push_back(0x00U);
  return payload;
}

}  // namespace

int main() {
  bool ok = true;

  {
    const std::vector<std::uint8_t> sample = makeSkewedSample();

    std::vector<std::uint8_t> compressed;
    const volt::io::HuffmanCodecResult encodeResult =
        volt::io::compressHuffmanData(sample, compressed);

    ok = expect(encodeResult.success, "compress skewed sample") && ok;
    ok = expect(!compressed.empty(), "compressed skewed sample has bytes") && ok;

    std::vector<std::uint8_t> decoded;
    const volt::io::HuffmanCodecResult decodeResult =
        volt::io::decompressHuffmanData(compressed, decoded);

    ok = expect(decodeResult.success, "decompress skewed sample") && ok;
    ok = expect(decoded == sample, "skewed sample roundtrip") && ok;
  }

  {
    const std::vector<std::uint8_t> sample = makeWideSample();

    std::vector<std::uint8_t> compressed;
    const volt::io::HuffmanCodecResult encodeResult =
        volt::io::compressHuffmanData(sample, compressed);
    ok = expect(encodeResult.success, "compress wide sample") && ok;
    ok = expect(compressed.size() > 6U, "wide sample header should exist") && ok;
    ok = expect((compressed[5] & kFlagCodeLengthRle) != 0U,
          "wide sample should enable code-length RLE") && ok;

    std::vector<std::uint8_t> decoded;
    const volt::io::HuffmanCodecResult decodeResult =
        volt::io::decompressHuffmanData(compressed, decoded);
    ok = expect(decodeResult.success, "decompress wide sample") && ok;
    ok = expect(decoded == sample, "wide sample roundtrip") && ok;
  }

    {
    const std::vector<std::uint8_t> sample = makeWideSample();
    volt::io::HuffmanEncodeOptions options{};
    options.preferCodeLengthRle = false;

    std::vector<std::uint8_t> compressed;
    const volt::io::HuffmanCodecResult encodeResult =
      volt::io::compressHuffmanData(sample, compressed, options);
    ok = expect(encodeResult.success, "compress wide sample without RLE") && ok;
    ok = expect(compressed.size() > 6U, "non-RLE wide sample header should exist") && ok;
    ok = expect((compressed[5] & kFlagCodeLengthRle) == 0U,
          "wide sample should disable code-length RLE when requested") && ok;

    std::vector<std::uint8_t> decoded;
    const volt::io::HuffmanCodecResult decodeResult =
      volt::io::decompressHuffmanData(compressed, decoded);
    ok = expect(decodeResult.success, "decompress non-RLE wide sample") && ok;
    ok = expect(decoded == sample, "non-RLE wide sample roundtrip") && ok;
    }

    {
    const std::vector<std::uint8_t> sample = makeDeepTreeSample();

    std::vector<std::uint8_t> compressed;
    const volt::io::HuffmanCodecResult encodeResult =
      volt::io::compressHuffmanData(sample, compressed);
    ok = expect(encodeResult.success, "compress deep-tree sample") && ok;

    std::vector<std::uint8_t> decoded;
    const volt::io::HuffmanCodecResult decodeResult =
      volt::io::decompressHuffmanData(compressed, decoded);
    ok = expect(decodeResult.success, "decompress deep-tree sample") && ok;
    ok = expect(decoded == sample, "deep-tree sample roundtrip") && ok;
    }

  {
    const std::vector<std::uint8_t> empty;
    std::vector<std::uint8_t> compressed;
    const volt::io::HuffmanCodecResult encodeResult =
        volt::io::compressHuffmanData(empty, compressed);
    ok = expect(encodeResult.success, "compress empty payload") && ok;

    std::vector<std::uint8_t> decoded;
    const volt::io::HuffmanCodecResult decodeResult =
        volt::io::decompressHuffmanData(compressed, decoded);
    ok = expect(decodeResult.success, "decompress empty payload") && ok;
    ok = expect(decoded.empty(), "empty payload roundtrip") && ok;
  }

  {
    const std::vector<std::uint8_t> sample = makeSkewedSample();
    std::vector<std::uint8_t> compressed;
    const volt::io::HuffmanCodecResult encodeResult =
        volt::io::compressHuffmanData(sample, compressed);
    ok = expect(encodeResult.success, "compress corruption fixture") && ok;

    if (!compressed.empty()) {
      compressed[0] = static_cast<std::uint8_t>('X');
      std::vector<std::uint8_t> decoded;
      const volt::io::HuffmanCodecResult decodeResult =
          volt::io::decompressHuffmanData(compressed, decoded);
      ok = expect(!decodeResult.success, "invalid magic should fail") && ok;
    } else {
      ok = expect(false, "compression fixture was empty") && ok;
    }
  }

  {
    const std::vector<std::uint8_t> sample = makeSkewedSample();
    std::vector<std::uint8_t> compressed;
    const volt::io::HuffmanCodecResult encodeResult =
        volt::io::compressHuffmanData(sample, compressed);
    ok = expect(encodeResult.success, "compress truncation fixture") && ok;

    if (compressed.size() > 1U) {
      compressed.pop_back();
      std::vector<std::uint8_t> decoded;
      const volt::io::HuffmanCodecResult decodeResult =
          volt::io::decompressHuffmanData(compressed, decoded);
      ok = expect(!decodeResult.success, "truncated bitstream should fail") && ok;
    } else {
      ok = expect(false, "compressed fixture too small for truncation") && ok;
    }
  }

  {
    std::vector<std::uint8_t> decoded;
    const volt::io::HuffmanCodecResult decodeResult =
        volt::io::decompressHuffmanData(makeOversubscribedHeader(), decoded);
    ok = expect(!decodeResult.success, "oversubscribed code lengths should fail") && ok;
  }

  {
    std::vector<std::uint8_t> decoded;
    const volt::io::HuffmanCodecResult decodeResult =
        volt::io::decompressHuffmanData(makeDuplicateSymbolHeader(), decoded);
    ok = expect(!decodeResult.success, "duplicate symbols should fail") && ok;
  }

  {
    const std::vector<std::uint8_t> sample = makeSkewedSample();
    std::vector<std::uint8_t> compressed;
    const volt::io::HuffmanCodecResult encodeResult =
        volt::io::compressHuffmanData(sample, compressed);
    ok = expect(encodeResult.success, "compress checksum fixture") && ok;

    if (!compressed.empty()) {
      compressed.back() ^= 0x01U;
      std::vector<std::uint8_t> decoded;
      const volt::io::HuffmanCodecResult decodeResult =
          volt::io::decompressHuffmanData(compressed, decoded);
      ok = expect(!decodeResult.success, "corrupted payload should fail") && ok;
    } else {
      ok = expect(false, "compressed fixture was empty for checksum test") && ok;
    }
  }

  {
    const std::vector<std::uint8_t> sample = makeWideSample();
    std::vector<std::uint8_t> compressed;
    const volt::io::HuffmanCodecResult encodeResult =
        volt::io::compressHuffmanData(sample, compressed);
    ok = expect(encodeResult.success, "compress RLE corruption fixture") && ok;

    if (compressed.size() > 22U && (compressed[5] & kFlagCodeLengthRle) != 0U) {
      compressed[20] = 0xFFU;
      compressed[21] = 0x7FU;

      std::vector<std::uint8_t> decoded;
      const volt::io::HuffmanCodecResult decodeResult =
          volt::io::decompressHuffmanData(compressed, decoded);
      ok = expect(!decodeResult.success, "invalid RLE table size should fail") && ok;
    } else {
      ok = expect(false, "RLE corruption fixture should include RLE table metadata") && ok;
    }
  }

  if (!ok) {
    std::cerr << "[huffman-test] One or more tests failed." << '\n';
    return 1;
  }

  std::cout << "[huffman-test] All Huffman codec tests passed." << '\n';
  return 0;
}
