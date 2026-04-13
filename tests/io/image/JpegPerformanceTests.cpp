#include "volt/io/image/ImageDecoder.hpp"
#include "volt/io/image/ImageEncoder.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[jpeg-perf-test] FAIL: " << message << '\n';
    return false;
  }
  return true;
}

volt::io::RawImage makePerfImage(std::uint32_t width, std::uint32_t height) {
  volt::io::RawImage image{};
  image.width = width;
  image.height = height;
  image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);

  for (std::uint32_t y = 0U; y < height; ++y) {
    for (std::uint32_t x = 0U; x < width; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x) * 4U;
      image.rgba[i + 0U] = static_cast<std::uint8_t>((x * 13U + y * 7U) & 0xFFU);
      image.rgba[i + 1U] = static_cast<std::uint8_t>((x * 3U + y * 17U) & 0xFFU);
      image.rgba[i + 2U] = static_cast<std::uint8_t>((x * 11U + y * 5U) & 0xFFU);
      image.rgba[i + 3U] = 255U;
    }
  }

  return image;
}

const std::array<std::array<double, 8>, 8>& dctCosTable() {
  static const std::array<std::array<double, 8>, 8> table = [] {
    std::array<std::array<double, 8>, 8> t{};
    constexpr double kPi = 3.14159265358979323846;
    for (int u = 0; u < 8; ++u) {
      for (int x = 0; x < 8; ++x) {
        t[static_cast<std::size_t>(u)][static_cast<std::size_t>(x)] =
            std::cos(((2.0 * x + 1.0) * u * kPi) / 16.0);
      }
    }
    return t;
  }();
  return table;
}

void forwardDctReference(const std::array<double, 64>& input,
                         std::array<double, 64>& output) {
  const auto& c = dctCosTable();
  constexpr double kInvSqrt2 = 0.7071067811865475244;

  for (int v = 0; v < 8; ++v) {
    for (int u = 0; u < 8; ++u) {
      double sum = 0.0;
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
          sum += input[static_cast<std::size_t>(y * 8 + x)] *
                 c[static_cast<std::size_t>(u)][static_cast<std::size_t>(x)] *
                 c[static_cast<std::size_t>(v)][static_cast<std::size_t>(y)];
        }
      }

      const double cu = (u == 0) ? kInvSqrt2 : 1.0;
      const double cv = (v == 0) ? kInvSqrt2 : 1.0;
      output[static_cast<std::size_t>(v * 8 + u)] = 0.25 * cu * cv * sum;
    }
  }
}

void forwardDctSeparable(const std::array<double, 64>& input,
                         std::array<double, 64>& output) {
  const auto& c = dctCosTable();
  constexpr std::array<double, 8> kAlpha = {
      0.7071067811865475244,
      1.0,
      1.0,
      1.0,
      1.0,
      1.0,
      1.0,
      1.0,
  };

  std::array<double, 64> tmp{};

  for (int y = 0; y < 8; ++y) {
    for (int u = 0; u < 8; ++u) {
      double sum = 0.0;
      for (int x = 0; x < 8; ++x) {
        sum += input[static_cast<std::size_t>(y * 8 + x)] *
               c[static_cast<std::size_t>(u)][static_cast<std::size_t>(x)];
      }
      tmp[static_cast<std::size_t>(y * 8 + u)] = sum;
    }
  }

  for (int v = 0; v < 8; ++v) {
    for (int u = 0; u < 8; ++u) {
      double sum = 0.0;
      for (int y = 0; y < 8; ++y) {
        sum += tmp[static_cast<std::size_t>(y * 8 + u)] *
               c[static_cast<std::size_t>(v)][static_cast<std::size_t>(y)];
      }
      output[static_cast<std::size_t>(v * 8 + u)] =
          0.25 * kAlpha[static_cast<std::size_t>(u)] *
          kAlpha[static_cast<std::size_t>(v)] * sum;
    }
  }
}

template <typename Fn>
double benchmarkTransform(Fn&& fn, std::size_t blocks, std::size_t iterations) {
  std::vector<std::array<double, 64>> inputBlocks(blocks);
  std::vector<std::array<double, 64>> outputBlocks(blocks);

  for (std::size_t b = 0U; b < blocks; ++b) {
    for (std::size_t i = 0U; i < 64U; ++i) {
      inputBlocks[b][i] = static_cast<double>((b * 17U + i * 13U) % 255U) - 128.0;
    }
  }

  const auto start = std::chrono::steady_clock::now();
  for (std::size_t it = 0U; it < iterations; ++it) {
    for (std::size_t b = 0U; b < blocks; ++b) {
      fn(inputBlocks[b], outputBlocks[b]);
    }
  }
  const auto end = std::chrono::steady_clock::now();

  volatile double guard = outputBlocks[0][0];
  (void)guard;

  return std::chrono::duration<double>(end - start).count();
}

double computeMeanAbsoluteRgbError(const volt::io::RawImage& lhs,
                                   const volt::io::RawImage& rhs) {
  if (lhs.width != rhs.width || lhs.height != rhs.height) {
    return 1e9;
  }

  const std::size_t pixelCount = static_cast<std::size_t>(lhs.width) * static_cast<std::size_t>(lhs.height);
  if (pixelCount == 0U) {
    return 0.0;
  }

  std::uint64_t totalError = 0U;
  for (std::size_t px = 0U; px < pixelCount; ++px) {
    const std::size_t i = px * 4U;
    totalError += static_cast<std::uint64_t>(std::abs(static_cast<int>(lhs.rgba[i + 0U]) - static_cast<int>(rhs.rgba[i + 0U])));
    totalError += static_cast<std::uint64_t>(std::abs(static_cast<int>(lhs.rgba[i + 1U]) - static_cast<int>(rhs.rgba[i + 1U])));
    totalError += static_cast<std::uint64_t>(std::abs(static_cast<int>(lhs.rgba[i + 2U]) - static_cast<int>(rhs.rgba[i + 2U])));
  }

  return static_cast<double>(totalError) / static_cast<double>(pixelCount * 3U);
}

double benchmarkEncode(const volt::io::RawImage& image,
                       std::size_t iterations,
                       std::vector<std::uint8_t>& outEncoded) {
  const auto tempPath = std::filesystem::temp_directory_path() / "volt-jpeg-perf.jpg";

  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0U; i < iterations; ++i) {
    if (!volt::io::encodeImageFile(tempPath, image, volt::io::ImageEncodeFormat::kJpeg, 90)) {
      return -1.0;
    }
  }
  const auto end = std::chrono::steady_clock::now();

  std::ifstream in(tempPath, std::ios::binary);
  if (!in.is_open()) {
    return -1.0;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) {
    return -1.0;
  }
  in.seekg(0, std::ios::beg);
  outEncoded.resize(static_cast<std::size_t>(size));
  if (!outEncoded.empty()) {
    in.read(reinterpret_cast<char*>(outEncoded.data()), size);
  }

  return std::chrono::duration<double>(end - start).count();
}

double benchmarkDecode(const std::filesystem::path& path,
                       std::size_t iterations,
                       volt::io::RawImage& decoded) {
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0U; i < iterations; ++i) {
    if (!volt::io::decodeImageFile(path, decoded)) {
      return -1.0;
    }
  }
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(end - start).count();
}

}  // namespace

int main() {
  bool ok = true;

  const std::size_t blocks = 512U;
  const std::size_t dctIterations = 64U;

  const double refSeconds = benchmarkTransform(forwardDctReference, blocks, dctIterations);
  const double fastSeconds = benchmarkTransform(forwardDctSeparable, blocks, dctIterations);

  ok = expect(refSeconds > 0.0 && fastSeconds > 0.0, "DCT benchmarks should produce valid timings") && ok;
  ok = expect(fastSeconds < refSeconds * 0.80, "separable DCT should be at least 20% faster than reference") && ok;

  std::cout << "[jpeg-perf-test] dct_reference_seconds=" << refSeconds
            << " dct_separable_seconds=" << fastSeconds
            << " speedup=" << (refSeconds / fastSeconds) << '\n';

  const volt::io::RawImage image = makePerfImage(256U, 256U);
  const std::filesystem::path jpegPath = std::filesystem::temp_directory_path() / "volt-jpeg-perf.jpg";

  const std::size_t encodeIterations = 8U;
  std::vector<std::uint8_t> encoded;
  const double encodeSeconds = benchmarkEncode(image, encodeIterations, encoded);
  ok = expect(encodeSeconds > 0.0, "JPEG encode benchmark should succeed") && ok;
  ok = expect(!encoded.empty(), "JPEG benchmark output should not be empty") && ok;

  volt::io::RawImage decoded{};
  const std::size_t decodeIterations = 16U;
  const double decodeSeconds = benchmarkDecode(jpegPath, decodeIterations, decoded);
  ok = expect(decodeSeconds > 0.0, "JPEG decode benchmark should succeed") && ok;

  ok = expect(decoded.width == image.width && decoded.height == image.height,
              "JPEG benchmark decode dimensions should match") && ok;

  const double mae = computeMeanAbsoluteRgbError(image, decoded);
  ok = expect(mae < 26.0, "JPEG benchmark decode quality should remain bounded") && ok;

  const double encodedMiB = (static_cast<double>(image.rgba.size()) * static_cast<double>(encodeIterations)) / (1024.0 * 1024.0);
  const double decodedMiB = (static_cast<double>(image.rgba.size()) * static_cast<double>(decodeIterations)) / (1024.0 * 1024.0);

  std::cout << "[jpeg-perf-test] encode_mib_per_s=" << (encodedMiB / encodeSeconds)
            << " decode_mib_per_s=" << (decodedMiB / decodeSeconds)
            << " mae=" << mae << '\n';

  if (!ok) {
    std::cerr << "[jpeg-perf-test] One or more checks failed." << '\n';
    return 1;
  }

  std::cout << "[jpeg-perf-test] All JPEG performance checks passed." << '\n';
  return 0;
}
