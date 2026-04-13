#include "volt/io/compression/HuffmanCodec.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::uint8_t kFlagCodeLengthRle = 0x02U;
constexpr std::size_t kTargetEncodeBytes = 8U * 1024U * 1024U;
constexpr std::size_t kTargetDecodeBytes = 16U * 1024U * 1024U;

volatile std::size_t gBenchmarkGuard = 0U;

struct Dataset {
  std::string name;
  std::vector<std::uint8_t> bytes;
};

std::vector<std::uint8_t> makeSkewedSample() {
  std::vector<std::uint8_t> data;
  data.reserve(1024U * 1024U);

  while (data.size() < 1024U * 1024U) {
    for (std::size_t i = 0U; i < 1200U; ++i) {
      data.push_back(static_cast<std::uint8_t>('A'));
    }
    for (std::size_t i = 0U; i < 300U; ++i) {
      data.push_back(static_cast<std::uint8_t>('B'));
    }
    for (std::size_t i = 0U; i < 120U; ++i) {
      data.push_back(static_cast<std::uint8_t>('C'));
    }
    for (std::size_t i = 0U; i < 64U; ++i) {
      data.push_back(static_cast<std::uint8_t>((i * 29U + 7U) & 0xFFU));
    }
  }

  data.resize(1024U * 1024U);
  return data;
}

std::vector<std::uint8_t> makeWideSample() {
  std::vector<std::uint8_t> data;
  data.reserve(1024U * 1024U);

  while (data.size() < 1024U * 1024U) {
    for (std::uint32_t round = 0U; round < 16U; ++round) {
      for (std::uint32_t symbol = 0U; symbol < 256U; ++symbol) {
        data.push_back(static_cast<std::uint8_t>((symbol + round * 17U) & 0xFFU));
      }
    }
  }

  data.resize(1024U * 1024U);
  return data;
}

std::vector<std::uint8_t> makeDeepTreeSample() {
  std::vector<std::uint8_t> seed;
  std::uint32_t prev = 1U;
  std::uint32_t curr = 1U;

  for (std::uint32_t symbol = 0U; symbol < 14U; ++symbol) {
    const std::uint32_t count = symbol < 2U ? 1U : curr;
    for (std::uint32_t i = 0U; i < count; ++i) {
      seed.push_back(static_cast<std::uint8_t>(symbol));
    }

    const std::uint32_t next = prev + curr;
    prev = curr;
    curr = next;
  }

  std::vector<std::uint8_t> data;
  data.reserve(1024U * 1024U);
  while (data.size() < 1024U * 1024U) {
    data.insert(data.end(), seed.begin(), seed.end());
  }

  data.resize(1024U * 1024U);
  return data;
}

std::vector<std::uint8_t> makeTextLikeSample() {
  const std::string lorem =
      "Volt compression benchmark stream. Huffman coding favors repeated tokens and punctuation. "
      "This synthetic corpus mixes words, whitespace, and repeated fragments for micro-bench coverage.\n";

  std::vector<std::uint8_t> data;
  data.reserve(1024U * 1024U);
  while (data.size() < 1024U * 1024U) {
    data.insert(data.end(), lorem.begin(), lorem.end());
  }

  data.resize(1024U * 1024U);
  return data;
}

std::size_t iterationsForBytes(std::size_t bytesPerIteration, std::size_t targetBytes) {
  if (bytesPerIteration == 0U) {
    return 1U;
  }
  return std::max<std::size_t>(1U, targetBytes / bytesPerIteration);
}

bool isRleHeader(const std::vector<std::uint8_t>& compressedFrame) {
  return compressedFrame.size() > 6U && (compressedFrame[5] & kFlagCodeLengthRle) != 0U;
}

template <typename Fn>
double timeRepeated(std::size_t iterations, Fn&& fn) {
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0U; i < iterations; ++i) {
    fn();
  }
  const auto stop = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(stop - start).count();
}

double mibPerSecond(std::size_t bytesPerIteration, std::size_t iterations, double seconds) {
  if (seconds <= 0.0) {
    return 0.0;
  }
  const double totalBytes = static_cast<double>(bytesPerIteration) * static_cast<double>(iterations);
  return totalBytes / (1024.0 * 1024.0) / seconds;
}

bool runDataset(const Dataset& dataset) {
  std::cout << "[bench] dataset=" << dataset.name
            << " input_bytes=" << dataset.bytes.size() << '\n';

  volt::io::HuffmanEncodeOptions defaultOptions{};
  volt::io::HuffmanEncodeOptions noRleOptions{};
  noRleOptions.preferCodeLengthRle = false;

  std::vector<std::uint8_t> compressedDefault;
  const volt::io::HuffmanCodecResult defaultEncode =
      volt::io::compressHuffmanData(dataset.bytes, compressedDefault, defaultOptions);
  if (!defaultEncode.success) {
    std::cerr << "[bench] FAIL: initial default encode failed: "
              << defaultEncode.errorMessage << '\n';
    return false;
  }

  std::vector<std::uint8_t> compressedNoRle;
  const volt::io::HuffmanCodecResult noRleEncode =
      volt::io::compressHuffmanData(dataset.bytes, compressedNoRle, noRleOptions);
  if (!noRleEncode.success) {
    std::cerr << "[bench] FAIL: initial no-RLE encode failed: "
              << noRleEncode.errorMessage << '\n';
    return false;
  }

  std::vector<std::uint8_t> decoded;
  const volt::io::HuffmanCodecResult defaultDecode =
      volt::io::decompressHuffmanData(compressedDefault, decoded);
  if (!defaultDecode.success || decoded != dataset.bytes) {
    std::cerr << "[bench] FAIL: initial default decode mismatch" << '\n';
    return false;
  }

  const volt::io::HuffmanCodecResult noRleDecode =
      volt::io::decompressHuffmanData(compressedNoRle, decoded);
  if (!noRleDecode.success || decoded != dataset.bytes) {
    std::cerr << "[bench] FAIL: initial no-RLE decode mismatch" << '\n';
    return false;
  }

  const std::size_t encodeIterations = iterationsForBytes(dataset.bytes.size(), kTargetEncodeBytes);
  const std::size_t decodeIterationsDefault = iterationsForBytes(compressedDefault.size(), kTargetDecodeBytes);
  const std::size_t decodeIterationsNoRle = iterationsForBytes(compressedNoRle.size(), kTargetDecodeBytes);

  bool encodeLoopOk = true;
  const double encodeDefaultSeconds = timeRepeated(encodeIterations, [&]() {
    std::vector<std::uint8_t> localCompressed;
    const volt::io::HuffmanCodecResult result =
        volt::io::compressHuffmanData(dataset.bytes, localCompressed, defaultOptions);
    if (!result.success) {
      encodeLoopOk = false;
      return;
    }
    gBenchmarkGuard += localCompressed.size();
  });

  const double encodeNoRleSeconds = timeRepeated(encodeIterations, [&]() {
    std::vector<std::uint8_t> localCompressed;
    const volt::io::HuffmanCodecResult result =
        volt::io::compressHuffmanData(dataset.bytes, localCompressed, noRleOptions);
    if (!result.success) {
      encodeLoopOk = false;
      return;
    }
    gBenchmarkGuard += localCompressed.size();
  });

  bool decodeLoopOk = true;
  const double decodeDefaultSeconds = timeRepeated(decodeIterationsDefault, [&]() {
    std::vector<std::uint8_t> localDecoded;
    const volt::io::HuffmanCodecResult result =
        volt::io::decompressHuffmanData(compressedDefault, localDecoded);
    if (!result.success || localDecoded != dataset.bytes) {
      decodeLoopOk = false;
      return;
    }
    gBenchmarkGuard += localDecoded.size();
  });

  const double decodeNoRleSeconds = timeRepeated(decodeIterationsNoRle, [&]() {
    std::vector<std::uint8_t> localDecoded;
    const volt::io::HuffmanCodecResult result =
        volt::io::decompressHuffmanData(compressedNoRle, localDecoded);
    if (!result.success || localDecoded != dataset.bytes) {
      decodeLoopOk = false;
      return;
    }
    gBenchmarkGuard += localDecoded.size();
  });

  if (!encodeLoopOk || !decodeLoopOk) {
    std::cerr << "[bench] FAIL: benchmark loop correctness failure" << '\n';
    return false;
  }

  const double ratioDefault = static_cast<double>(compressedDefault.size()) / static_cast<double>(dataset.bytes.size());
  const double ratioNoRle = static_cast<double>(compressedNoRle.size()) / static_cast<double>(dataset.bytes.size());

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  table_default=" << (isRleHeader(compressedDefault) ? "rle" : "pairs")
            << " size_default=" << compressedDefault.size()
            << " ratio_default=" << ratioDefault
            << " size_no_rle=" << compressedNoRle.size()
            << " ratio_no_rle=" << ratioNoRle << '\n';

  std::cout << "  encode_default_mib_s="
            << mibPerSecond(dataset.bytes.size(), encodeIterations, encodeDefaultSeconds)
            << " encode_no_rle_mib_s="
            << mibPerSecond(dataset.bytes.size(), encodeIterations, encodeNoRleSeconds)
            << '\n';

  std::cout << "  decode_default_mib_s="
            << mibPerSecond(compressedDefault.size(), decodeIterationsDefault, decodeDefaultSeconds)
            << " decode_no_rle_mib_s="
            << mibPerSecond(compressedNoRle.size(), decodeIterationsNoRle, decodeNoRleSeconds)
            << '\n';

  return true;
}

}  // namespace

int main() {
  std::vector<Dataset> datasets;
  datasets.push_back({"skewed", makeSkewedSample()});
  datasets.push_back({"wide", makeWideSample()});
  datasets.push_back({"deep-tree", makeDeepTreeSample()});
  datasets.push_back({"text-like", makeTextLikeSample()});

  bool ok = true;
  for (const Dataset& dataset : datasets) {
    ok = runDataset(dataset) && ok;
  }

  std::cout << "[bench] guard=" << gBenchmarkGuard << '\n';

  if (!ok) {
    std::cerr << "[bench] One or more benchmark scenarios failed." << '\n';
    return 1;
  }

  std::cout << "[bench] Huffman benchmark completed." << '\n';
  return 0;
}
