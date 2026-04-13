#include "volt/io/image/ImageDecoder.hpp"
#include "volt/io/image/ImageEncoder.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[image-codec-test] FAIL: " << message << '\n';
    return false;
  }
  return true;
}

volt::io::RawImage makeSampleImage() {
  volt::io::RawImage image{};
  image.width = 32U;
  image.height = 24U;
  image.rgba.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4U);

  for (std::uint32_t y = 0U; y < image.height; ++y) {
    for (std::uint32_t x = 0U; x < image.width; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + x) * 4U;
      image.rgba[i + 0U] = static_cast<std::uint8_t>((x * 255U) / image.width);
      image.rgba[i + 1U] = static_cast<std::uint8_t>((y * 255U) / image.height);
      image.rgba[i + 2U] = static_cast<std::uint8_t>((x + y) & 0xFFU);
      image.rgba[i + 3U] = static_cast<std::uint8_t>(255U - ((x * 3U + y * 5U) & 0x7FU));
    }
  }

  return image;
}

volt::io::RawImage makeTinyImage() {
  volt::io::RawImage image{};
  image.width = 1U;
  image.height = 1U;
  image.rgba = {17U, 34U, 51U, 68U};
  return image;
}

volt::io::RawImage makeLargeImage() {
  volt::io::RawImage image{};
  image.width = 192U;
  image.height = 128U;
  image.rgba.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4U);

  for (std::uint32_t y = 0U; y < image.height; ++y) {
    for (std::uint32_t x = 0U; x < image.width; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + x) * 4U;
      image.rgba[i + 0U] = static_cast<std::uint8_t>((x * 5U + y * 3U) & 0xFFU);
      image.rgba[i + 1U] = static_cast<std::uint8_t>((x * 11U + y * 7U) & 0xFFU);
      image.rgba[i + 2U] = static_cast<std::uint8_t>((x * 13U + y * 17U) & 0xFFU);
      image.rgba[i + 3U] = 255U;
    }
  }

  return image;
}

bool readFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }

  in.seekg(0, std::ios::end);
  const std::streamoff length = in.tellg();
  if (length < 0) {
    return false;
  }
  in.seekg(0, std::ios::beg);

  out.resize(static_cast<std::size_t>(length));
  if (!out.empty()) {
    in.read(reinterpret_cast<char*>(out.data()), length);
  }

  return in.good() || in.eof();
}

bool writeFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }

  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

bool containsJpegMarker(const std::vector<std::uint8_t>& bytes, std::uint8_t marker) {
  if (bytes.size() < 2U) {
    return false;
  }

  for (std::size_t i = 0U; i + 1U < bytes.size(); ++i) {
    if (bytes[i] == 0xFFU && bytes[i + 1U] == marker) {
      return true;
    }
  }

  return false;
}

bool containsAnyJpegRestartMarker(const std::vector<std::uint8_t>& bytes) {
  for (std::uint8_t marker = 0xD0U; marker <= 0xD7U; ++marker) {
    if (containsJpegMarker(bytes, marker)) {
      return true;
    }
  }
  return false;
}

bool jpegHas422Sampling(const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < 4U || bytes[0] != 0xFFU || bytes[1] != 0xD8U) {
    return false;
  }

  std::size_t i = 2U;
  while (i + 3U < bytes.size()) {
    if (bytes[i] != 0xFFU) {
      ++i;
      continue;
    }

    while (i < bytes.size() && bytes[i] == 0xFFU) {
      ++i;
    }
    if (i >= bytes.size()) {
      return false;
    }

    const std::uint8_t marker = bytes[i++];
    if (marker == 0x00U || marker == 0xFFU) {
      continue;
    }

    if (marker == 0xD8U || marker == 0xD9U || (marker >= 0xD0U && marker <= 0xD7U)) {
      continue;
    }

    if (i + 1U >= bytes.size()) {
      return false;
    }

    const std::uint16_t segmentLength =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[i]) << 8U) |
                                   static_cast<std::uint16_t>(bytes[i + 1U]));
    if (segmentLength < 2U || i + static_cast<std::size_t>(segmentLength) > bytes.size()) {
      return false;
    }

    if (marker == 0xC0U) {
      const std::size_t payload = i + 2U;
      if (payload + 14U > i + segmentLength) {
        return false;
      }

      const std::uint8_t componentCount = bytes[payload + 5U];
      if (componentCount != 3U) {
        return false;
      }

      const std::uint8_t yHv = bytes[payload + 7U];
      const std::uint8_t cbHv = bytes[payload + 10U];
      const std::uint8_t crHv = bytes[payload + 13U];
      return yHv == 0x21U && cbHv == 0x11U && crHv == 0x11U;
    }

    i += static_cast<std::size_t>(segmentLength);
  }

  return false;
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

}  // namespace

int main() {
  bool ok = true;

  const std::filesystem::path outputDir =
      std::filesystem::temp_directory_path() / "volt-image-codec-tests";
  std::error_code ec;
  std::filesystem::create_directories(outputDir, ec);

  const volt::io::RawImage source = makeSampleImage();
  const volt::io::RawImage tiny = makeTinyImage();

  {
    const std::filesystem::path pngPath = outputDir / "roundtrip.png";
    const bool encoded = volt::io::encodeImageFile(pngPath, source, volt::io::ImageEncodeFormat::kPng);
    ok = expect(encoded, "PNG encode should succeed") && ok;

    volt::io::RawImage decoded{};
    const bool decodedOk = encoded && volt::io::decodeImageFile(pngPath, decoded);
    ok = expect(decodedOk, "PNG decode should succeed") && ok;
    ok = expect(decoded.width == source.width && decoded.height == source.height, "PNG dimensions should roundtrip") && ok;
    ok = expect(decoded.rgba == source.rgba, "PNG pixels should roundtrip exactly") && ok;
  }

  {
    const std::filesystem::path bmpPath = outputDir / "roundtrip.bmp";
    const bool encoded = volt::io::encodeImageFile(bmpPath, source, volt::io::ImageEncodeFormat::kBmp);
    ok = expect(encoded, "BMP encode should succeed") && ok;

    volt::io::RawImage decoded{};
    const bool decodedOk = encoded && volt::io::decodeImageFile(bmpPath, decoded);
    ok = expect(decodedOk, "BMP decode should succeed") && ok;
    ok = expect(decoded.width == source.width && decoded.height == source.height, "BMP dimensions should roundtrip") && ok;
    ok = expect(decoded.rgba == source.rgba, "BMP pixels should roundtrip exactly") && ok;
  }

  {
    const std::filesystem::path tinyPngPath = outputDir / "tiny-roundtrip.png";
    const bool encodedPng = volt::io::encodeImageFile(tinyPngPath, tiny, volt::io::ImageEncodeFormat::kPng);
    ok = expect(encodedPng, "tiny PNG encode should succeed") && ok;

    volt::io::RawImage decodedPng{};
    const bool decodedPngOk = encodedPng && volt::io::decodeImageFile(tinyPngPath, decodedPng);
    ok = expect(decodedPngOk, "tiny PNG decode should succeed") && ok;
    ok = expect(decodedPng.rgba == tiny.rgba, "tiny PNG pixels should roundtrip exactly") && ok;

    const std::filesystem::path tinyBmpPath = outputDir / "tiny-roundtrip.bmp";
    const bool encodedBmp = volt::io::encodeImageFile(tinyBmpPath, tiny, volt::io::ImageEncodeFormat::kBmp);
    ok = expect(encodedBmp, "tiny BMP encode should succeed") && ok;

    volt::io::RawImage decodedBmp{};
    const bool decodedBmpOk = encodedBmp && volt::io::decodeImageFile(tinyBmpPath, decodedBmp);
    ok = expect(decodedBmpOk, "tiny BMP decode should succeed") && ok;
    ok = expect(decodedBmp.rgba == tiny.rgba, "tiny BMP pixels should roundtrip exactly") && ok;
  }

  {
    const std::filesystem::path goodPngPath = outputDir / "crc-good.png";
    const bool encoded = volt::io::encodeImageFile(goodPngPath, source, volt::io::ImageEncodeFormat::kPng);
    ok = expect(encoded, "PNG fixture encode should succeed") && ok;

    std::vector<std::uint8_t> pngBytes;
    const bool readOk = encoded && readFileBytes(goodPngPath, pngBytes);
    ok = expect(readOk, "PNG fixture should be readable") && ok;

    if (readOk && pngBytes.size() > 29U) {
      pngBytes[29U] ^= 0x01U;
      const std::filesystem::path badPngPath = outputDir / "crc-bad.png";
      const bool wrote = writeFileBytes(badPngPath, pngBytes);
      ok = expect(wrote, "corrupt PNG fixture should be writable") && ok;

      volt::io::RawImage decoded{};
      const bool decodedOk = wrote && volt::io::decodeImageFile(badPngPath, decoded);
      ok = expect(!decodedOk, "PNG decode should fail on CRC mismatch") && ok;
    } else {
      ok = expect(false, "PNG fixture was unexpectedly short") && ok;
    }
  }

  {
    const std::filesystem::path goodBmpPath = outputDir / "trunc-good.bmp";
    const bool encoded = volt::io::encodeImageFile(goodBmpPath, source, volt::io::ImageEncodeFormat::kBmp);
    ok = expect(encoded, "BMP fixture encode should succeed") && ok;

    std::vector<std::uint8_t> bmpBytes;
    const bool readOk = encoded && readFileBytes(goodBmpPath, bmpBytes);
    ok = expect(readOk, "BMP fixture should be readable") && ok;

    if (readOk && bmpBytes.size() > 60U) {
      bmpBytes.resize(bmpBytes.size() - 7U);
      const std::filesystem::path badBmpPath = outputDir / "trunc-bad.bmp";
      const bool wrote = writeFileBytes(badBmpPath, bmpBytes);
      ok = expect(wrote, "truncated BMP fixture should be writable") && ok;

      volt::io::RawImage decoded{};
      const bool decodedOk = wrote && volt::io::decodeImageFile(badBmpPath, decoded);
      ok = expect(!decodedOk, "BMP decode should fail on truncated pixel data") && ok;
    } else {
      ok = expect(false, "BMP fixture was unexpectedly short") && ok;
    }
  }

  {
    const std::filesystem::path jpgPath = outputDir / "roundtrip.jpg";
    const bool encoded = volt::io::encodeImageFile(jpgPath, source, volt::io::ImageEncodeFormat::kJpeg, 90);
    ok = expect(encoded, "JPEG encode should succeed") && ok;

    volt::io::RawImage decoded{};
    const bool decodedOk = encoded && volt::io::decodeImageFile(jpgPath, decoded);
    ok = expect(decodedOk, "JPEG decode should succeed after encode") && ok;
    ok = expect(decoded.width == source.width && decoded.height == source.height, "JPEG dimensions should roundtrip") && ok;

    const double mae = computeMeanAbsoluteRgbError(source, decoded);
    ok = expect(mae < 24.0, "JPEG RGB mean absolute error should remain bounded") && ok;
  }

  {
    const volt::io::RawImage large = makeLargeImage();
    const std::filesystem::path jpgPath = outputDir / "roundtrip-422-restart.jpg";
    const bool encoded = volt::io::encodeImageFile(jpgPath, large, volt::io::ImageEncodeFormat::kJpeg, 90);
    ok = expect(encoded, "JPEG 4:2:2+restart encode should succeed") && ok;

    std::vector<std::uint8_t> jpgBytes;
    const bool readOk = encoded && readFileBytes(jpgPath, jpgBytes);
    ok = expect(readOk, "JPEG 4:2:2+restart output should be readable") && ok;
    ok = expect(readOk && jpegHas422Sampling(jpgBytes), "JPEG should advertise 4:2:2 SOF0 sampling") && ok;
    ok = expect(readOk && containsJpegMarker(jpgBytes, 0xDDU), "JPEG should include DRI marker") && ok;
    ok = expect(readOk && containsAnyJpegRestartMarker(jpgBytes), "JPEG should include restart markers") && ok;

    volt::io::RawImage decoded{};
    const bool decodedOk = encoded && volt::io::decodeImageFile(jpgPath, decoded);
    ok = expect(decodedOk, "JPEG decode should handle restart markers") && ok;
    ok = expect(decoded.width == large.width && decoded.height == large.height,
                "JPEG restart decode dimensions should match") && ok;
    const double mae = computeMeanAbsoluteRgbError(large, decoded);
    ok = expect(mae < 30.0, "JPEG 4:2:2+restart decode quality should remain bounded") && ok;
  }

  {
    volt::io::RawImage oversized{};
    oversized.width = 70000U;
    oversized.height = 1U;
    oversized.rgba.assign(static_cast<std::size_t>(oversized.width) * 4U, 127U);

    const std::filesystem::path jpgPath = outputDir / "oversized.jpg";
    const bool encoded = volt::io::encodeImageFile(jpgPath, oversized, volt::io::ImageEncodeFormat::kJpeg, 90);
    ok = expect(!encoded, "JPEG encode should reject SOF0-oversized dimensions") && ok;
  }

  if (!ok) {
    std::cerr << "[image-codec-test] One or more tests failed." << '\n';
    return 1;
  }

  std::cout << "[image-codec-test] All image codec tests passed." << '\n';
  return 0;
}
