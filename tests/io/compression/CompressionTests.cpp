#include "volt/io/compression/DeflateCodec.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[compression-test] FAIL: " << message << '\n';
    return false;
  }
  return true;
}

std::vector<std::uint8_t> makeSampleData() {
  std::vector<std::uint8_t> data;
  data.reserve(4096);
  for (std::uint32_t i = 0; i < 4096U; ++i) {
    const std::uint8_t value = static_cast<std::uint8_t>((i * 37U + 13U) & 0xFFU);
    data.push_back(value);
  }
  return data;
}

std::vector<std::uint8_t> makeHighlyCompressibleData() {
  std::vector<std::uint8_t> data;
  data.reserve(32768U);

  for (std::size_t i = 0U; i < 8192U; ++i) {
    data.push_back(static_cast<std::uint8_t>('A'));
    data.push_back(static_cast<std::uint8_t>('B'));
    data.push_back(static_cast<std::uint8_t>('A'));
    data.push_back(static_cast<std::uint8_t>('B'));
  }

  return data;
}

std::vector<std::uint8_t> makeRawStoredDeflate(const std::vector<std::uint8_t>& input) {
  std::vector<std::uint8_t> output;
  if (input.empty()) {
    return output;
  }

  constexpr std::size_t kMaxStoredBlock = 65535U;

  std::size_t offset = 0U;
  while (offset < input.size()) {
    const std::size_t remaining = input.size() - offset;
    const std::size_t blockSize = std::min(kMaxStoredBlock, remaining);
    const bool finalBlock = (offset + blockSize) == input.size();

    output.push_back(finalBlock ? 0x01U : 0x00U);

    const std::uint16_t len = static_cast<std::uint16_t>(blockSize);
    const std::uint16_t nlen = static_cast<std::uint16_t>(~len);

    output.push_back(static_cast<std::uint8_t>(len & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((len >> 8U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>(nlen & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((nlen >> 8U) & 0xFFU));

    output.insert(
        output.end(),
        input.begin() + static_cast<std::ptrdiff_t>(offset),
        input.begin() + static_cast<std::ptrdiff_t>(offset + blockSize));

    offset += blockSize;
  }

  return output;
}

std::vector<std::uint8_t> makeDynamicHuffmanSample() {
  std::vector<std::uint8_t> data;
  data.reserve(3000U);
  for (std::size_t i = 0U; i < 600U; ++i) {
    data.push_back(static_cast<std::uint8_t>('a'));
    data.push_back(static_cast<std::uint8_t>('b'));
    data.push_back(static_cast<std::uint8_t>('c'));
    data.push_back(static_cast<std::uint8_t>('d'));
    data.push_back(static_cast<std::uint8_t>('e'));
  }
  return data;
}

std::vector<std::uint8_t> makeDynamicHuffmanZlibFixture() {
  return {
      0x78U, 0x9CU, 0xEDU, 0xC4U, 0x39U, 0x01U, 0x00U, 0x00U, 0x08U,
      0x04U, 0xA0U, 0xACU, 0x3EU, 0xD7U, 0xBFU, 0x82U, 0x35U, 0x1CU,
      0x60U, 0xA0U, 0x7AU, 0x36U, 0x25U, 0x49U, 0x92U, 0x24U, 0x29U,
      0x8FU, 0x3BU, 0x91U, 0x3BU, 0x88U, 0x65U,
  };
}

std::vector<std::uint8_t> makeMalformedZlibFcheckFixture(const std::vector<std::uint8_t>& input) {
  std::vector<std::uint8_t> frame;
  const volt::io::CompressionResult result = volt::io::compressZlibData(input, frame, 6);
  if (!result.success || frame.size() < 2U) {
    return {};
  }

  frame[1] ^= 0x01U;
  return frame;
}

std::vector<std::uint8_t> makeMalformedZlibFdictFixture(const std::vector<std::uint8_t>& input) {
  std::vector<std::uint8_t> frame;
  const volt::io::CompressionResult result = volt::io::compressZlibData(input, frame, 6);
  if (!result.success || frame.size() < 2U) {
    return {};
  }

  frame[1] = static_cast<std::uint8_t>(frame[1] | 0x20U);
  return frame;
}

std::vector<std::uint8_t> makeBadAdlerFixture(const std::vector<std::uint8_t>& input) {
  std::vector<std::uint8_t> frame;
  const volt::io::CompressionResult result = volt::io::compressZlibData(input, frame, 6);
  if (!result.success || frame.size() < 4U) {
    return {};
  }

  frame[frame.size() - 1U] ^= 0x01U;
  return frame;
}

}  // namespace

int main() {
  bool ok = true;

  const std::vector<std::uint8_t> sample = makeSampleData();

  {
    std::vector<std::uint8_t> compressed;
    const volt::io::CompressionResult compressResult =
        volt::io::compressZlibData(sample, compressed, 6);
    ok = expect(compressResult.success, "zlib compression should succeed") && ok;
    ok = expect(!compressed.empty(), "zlib compression should produce bytes") && ok;

    std::vector<std::uint8_t> decompressed;
    const volt::io::CompressionResult decompressResult =
        volt::io::decompressData(compressed, decompressed, volt::io::CompressionContainer::kZlib);
    ok = expect(decompressResult.success, "zlib decompression should succeed") && ok;
    ok = expect(decompressed == sample, "zlib roundtrip should match original data") && ok;
  }

  {
    const std::vector<std::uint8_t> compressible = makeHighlyCompressibleData();

    std::vector<std::uint8_t> compressed;
    const volt::io::CompressionResult compressResult =
        volt::io::compressZlibData(compressible, compressed, 9);
    ok = expect(compressResult.success, "compressible zlib compression should succeed") && ok;
    ok = expect(compressed.size() < compressible.size(), "compressible input should shrink") && ok;

    std::vector<std::uint8_t> decompressed;
    const volt::io::CompressionResult decompressResult =
        volt::io::decompressData(compressed, decompressed, volt::io::CompressionContainer::kZlib);
    ok = expect(decompressResult.success, "compressible zlib decompression should succeed") && ok;
    ok = expect(decompressed == compressible, "compressible roundtrip should match") && ok;
  }

  {
    const std::vector<std::uint8_t> rawDeflate = makeRawStoredDeflate(sample);
    ok = expect(!rawDeflate.empty(), "raw deflate fixture generation should succeed") && ok;

    std::vector<std::uint8_t> decompressed;
    const volt::io::CompressionResult rawResult =
        volt::io::decompressData(rawDeflate, decompressed, volt::io::CompressionContainer::kRawDeflate);
    ok = expect(rawResult.success, "raw deflate decompression should succeed") && ok;
    ok = expect(decompressed == sample, "raw deflate roundtrip should match original data") && ok;
  }

  {
    const std::vector<std::uint8_t> invalid{0x00U, 0xFFU, 0x12U, 0x34U};
    std::vector<std::uint8_t> out;
    const volt::io::CompressionResult invalidResult =
        volt::io::decompressData(invalid, out, volt::io::CompressionContainer::kZlib);
    ok = expect(!invalidResult.success, "invalid zlib payload should fail") && ok;
  }

  {
    const std::vector<std::uint8_t> badFcheck = makeMalformedZlibFcheckFixture(sample);
    ok = expect(!badFcheck.empty(), "bad FCHK fixture generation should succeed") && ok;

    std::vector<std::uint8_t> out;
    const volt::io::CompressionResult result =
        volt::io::decompressData(badFcheck, out, volt::io::CompressionContainer::kZlib);
    ok = expect(!result.success, "zlib bad FCHK header should fail") && ok;
  }

  {
    const std::vector<std::uint8_t> badFdict = makeMalformedZlibFdictFixture(sample);
    ok = expect(!badFdict.empty(), "bad FDICT fixture generation should succeed") && ok;

    std::vector<std::uint8_t> out;
    const volt::io::CompressionResult result =
        volt::io::decompressData(badFdict, out, volt::io::CompressionContainer::kZlib);
    ok = expect(!result.success, "zlib FDICT header should fail") && ok;
  }

  {
    const std::vector<std::uint8_t> badAdler = makeBadAdlerFixture(sample);
    ok = expect(!badAdler.empty(), "bad adler fixture generation should succeed") && ok;

    std::vector<std::uint8_t> out;
    const volt::io::CompressionResult result =
        volt::io::decompressData(badAdler, out, volt::io::CompressionContainer::kZlib);
    ok = expect(!result.success, "zlib adler mismatch should fail") && ok;
  }

  {
    const std::vector<std::uint8_t> expected = makeDynamicHuffmanSample();
    const std::vector<std::uint8_t> fixture = makeDynamicHuffmanZlibFixture();

    std::vector<std::uint8_t> decompressed;
    const volt::io::CompressionResult dynamicResult =
        volt::io::decompressData(fixture, decompressed, volt::io::CompressionContainer::kZlib);
    ok = expect(dynamicResult.success, "dynamic-huffman zlib fixture should decompress") && ok;
    ok = expect(decompressed == expected, "dynamic-huffman zlib fixture should match expected bytes") && ok;
  }

  if (!ok) {
    std::cerr << "[compression-test] One or more tests failed." << '\n';
    return 1;
  }

  std::cout << "[compression-test] All compression tests passed." << '\n';
  return 0;
}
