#include "PngCodec.hpp"

#include "volt/io/compression/DeflateCodec.hpp"

#include "CodecShared.hpp"

#include <array>
#include <cstring>
#include <limits>

namespace {

constexpr std::size_t kMaxDecodedImageBytes = 256U * 1024U * 1024U;
constexpr std::array<char, 4> kChunkIhdr = {'I', 'H', 'D', 'R'};
constexpr std::array<char, 4> kChunkIdat = {'I', 'D', 'A', 'T'};
constexpr std::array<char, 4> kChunkIend = {'I', 'E', 'N', 'D'};

bool isPngChunkTypeValid(const std::array<char, 4>& type) {
  for (char ch : type) {
    const std::uint8_t byte = static_cast<std::uint8_t>(ch);
    const bool isUpper = byte >= static_cast<std::uint8_t>('A') && byte <= static_cast<std::uint8_t>('Z');
    const bool isLower = byte >= static_cast<std::uint8_t>('a') && byte <= static_cast<std::uint8_t>('z');
    if (!isUpper && !isLower) {
      return false;
    }
  }

  // Reserved bit (third character) must be uppercase per PNG spec.
  return (static_cast<std::uint8_t>(type[2]) & 0x20U) == 0U;
}

std::uint64_t filterByteCost(std::uint8_t value) {
  const int signedValue = static_cast<std::int8_t>(value);
  return static_cast<std::uint64_t>(signedValue < 0 ? -signedValue : signedValue);
}

}  // namespace

namespace volt::io::codec {

bool decodePng(const std::vector<std::uint8_t>& bytes, RawImage& outImage, std::string& error) {
  if (!hasPngSignature(bytes)) {
    error = "png signature missing";
    return false;
  }

  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::uint8_t bitDepth = 0U;
  std::uint8_t colorType = 0U;
  std::uint8_t compressionMethod = 0U;
  std::uint8_t filterMethod = 0U;
  std::uint8_t interlaceMethod = 0U;

  bool sawIhdr = false;
  bool sawIend = false;
  bool sawIdat = false;
  bool sawNonIdatAfterIdat = false;
  std::vector<std::uint8_t> idat;

  std::size_t offset = 8U;
  while (offset + 12U <= bytes.size()) {
    std::uint32_t chunkLength = 0U;
    if (!readU32Be(bytes, offset, chunkLength)) {
      error = "png chunk length truncated";
      return false;
    }
    offset += 4U;

    if (offset + 4U > bytes.size()) {
      error = "png chunk type truncated";
      return false;
    }

    const std::size_t chunkTypeOffset = offset;
    const std::array<char, 4> chunkType = {
        static_cast<char>(bytes[offset + 0U]),
        static_cast<char>(bytes[offset + 1U]),
        static_cast<char>(bytes[offset + 2U]),
        static_cast<char>(bytes[offset + 3U]),
    };
    offset += 4U;

    if (!isPngChunkTypeValid(chunkType)) {
      error = "png chunk type invalid";
      return false;
    }

    if (offset + 4U > bytes.size() ||
        static_cast<std::size_t>(chunkLength) > bytes.size() - offset - 4U) {
      error = "png chunk payload truncated";
      return false;
    }

    const std::size_t chunkDataOffset = offset;
    const std::size_t chunkCrcOffset = chunkDataOffset + static_cast<std::size_t>(chunkLength);
    std::uint32_t chunkCrc = 0U;
    if (!readU32Be(bytes, chunkCrcOffset, chunkCrc)) {
      error = "png chunk crc truncated";
      return false;
    }

    const std::uint32_t computedCrc =
        crc32(bytes.data() + chunkTypeOffset, 4U + static_cast<std::size_t>(chunkLength));
    if (chunkCrc != computedCrc) {
      error = "png chunk crc mismatch";
      return false;
    }

    offset = chunkCrcOffset + 4U;

    if (!sawIhdr && chunkType != kChunkIhdr) {
      error = "png IHDR must be first chunk";
      return false;
    }

    if (chunkType == kChunkIhdr) {
      if (sawIhdr) {
        error = "png duplicate IHDR chunk";
        return false;
      }

      if (chunkLength != 13U) {
        error = "png IHDR size invalid";
        return false;
      }

      if (!readU32Be(bytes, chunkDataOffset + 0U, width) ||
          !readU32Be(bytes, chunkDataOffset + 4U, height)) {
        error = "png IHDR dimensions truncated";
        return false;
      }

      bitDepth = bytes[chunkDataOffset + 8U];
      colorType = bytes[chunkDataOffset + 9U];
      compressionMethod = bytes[chunkDataOffset + 10U];
      filterMethod = bytes[chunkDataOffset + 11U];
      interlaceMethod = bytes[chunkDataOffset + 12U];
      sawIhdr = true;
    } else if (chunkType == kChunkIdat) {
      if (sawNonIdatAfterIdat) {
        error = "png IDAT chunks must be consecutive";
        return false;
      }
      sawIdat = true;
      if (idat.size() > std::numeric_limits<std::size_t>::max() - chunkLength) {
        error = "png IDAT data too large";
        return false;
      }
      idat.insert(
          idat.end(),
          bytes.begin() + static_cast<std::ptrdiff_t>(chunkDataOffset),
          bytes.begin() + static_cast<std::ptrdiff_t>(chunkDataOffset + chunkLength));
    } else if (chunkType == kChunkIend) {
      if (chunkLength != 0U) {
        error = "png IEND size invalid";
        return false;
      }
      sawIend = true;
      break;
    } else if (sawIdat) {
      sawNonIdatAfterIdat = true;
    }
  }

  if (!sawIhdr || !sawIend || !sawIdat || idat.empty()) {
    error = "png missing required chunks";
    return false;
  }

  if (offset != bytes.size()) {
    error = "png trailing data after IEND";
    return false;
  }

  if (width == 0U || height == 0U || bitDepth != 8U || compressionMethod != 0U ||
      filterMethod != 0U || interlaceMethod != 0U) {
    error = "png header parameters unsupported";
    return false;
  }

  int channels = 0;
  if (colorType == 6U) {
    channels = 4;
  } else if (colorType == 2U) {
    channels = 3;
  } else {
    error = "png color type unsupported";
    return false;
  }

  if (height != 0U && width > std::numeric_limits<std::size_t>::max() / height) {
    error = "png dimensions overflow";
    return false;
  }
  const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (pixelCount > kMaxDecodedImageBytes / 4U) {
    error = "png dimensions exceed safety limit";
    return false;
  }

  const std::size_t bytesPerPixel = static_cast<std::size_t>(channels);
  if (width != 0U && bytesPerPixel > std::numeric_limits<std::size_t>::max() / width) {
    error = "png dimensions overflow";
    return false;
  }
  const std::size_t rowBytes = static_cast<std::size_t>(width) * bytesPerPixel;
  if (height != 0U && rowBytes > std::numeric_limits<std::size_t>::max() / height) {
    error = "png dimensions overflow";
    return false;
  }
  const std::size_t unfilteredBytes = static_cast<std::size_t>(height) * rowBytes;
  if (unfilteredBytes > kMaxDecodedImageBytes) {
    error = "png dimensions exceed safety limit";
    return false;
  }

  if (rowBytes > std::numeric_limits<std::size_t>::max() - 1U) {
    error = "png dimensions overflow";
    return false;
  }
  const std::size_t rowWithFilter = rowBytes + 1U;
  if (height != 0U && rowWithFilter > std::numeric_limits<std::size_t>::max() / height) {
    error = "png dimensions overflow";
    return false;
  }
  const std::size_t expectedBytes = static_cast<std::size_t>(height) * rowWithFilter;

  std::vector<std::uint8_t> inflated;
  const CompressionResult inflateResult =
      decompressData(idat, inflated, CompressionContainer::kZlib);
  if (!inflateResult.success) {
    error = "png idat deflate decode failed";
    return false;
  }

  if (inflated.size() != expectedBytes) {
    error = "png scanline data size mismatch";
    return false;
  }

  std::vector<std::uint8_t> unfiltered(unfilteredBytes, 0U);

  const std::uint8_t* inflatedData = inflated.data();
  std::uint8_t* unfilteredData = unfiltered.data();
  const std::size_t prefixBytes = std::min(bytesPerPixel, rowBytes);

  std::size_t srcOffset = 0U;
  for (std::uint32_t y = 0U; y < height; ++y) {
    const std::uint8_t filterType = inflatedData[srcOffset++];
    const std::size_t rowOffset = static_cast<std::size_t>(y) * rowBytes;
    const std::uint8_t* srcRow = inflatedData + srcOffset;
    std::uint8_t* dstRow = unfilteredData + rowOffset;
    const std::uint8_t* prevRow = y > 0U ? dstRow - rowBytes : nullptr;

    switch (filterType) {
      case 0U:
        std::memcpy(dstRow, srcRow, rowBytes);
        break;
      case 1U: {
        for (std::size_t x = 0U; x < prefixBytes; ++x) {
          dstRow[x] = srcRow[x];
        }
        for (std::size_t x = prefixBytes; x < rowBytes; ++x) {
          dstRow[x] = static_cast<std::uint8_t>(srcRow[x] + dstRow[x - bytesPerPixel]);
        }
        break;
      }
      case 2U:
        if (prevRow == nullptr) {
          std::memcpy(dstRow, srcRow, rowBytes);
        } else {
          for (std::size_t x = 0U; x < rowBytes; ++x) {
            dstRow[x] = static_cast<std::uint8_t>(srcRow[x] + prevRow[x]);
          }
        }
        break;
      case 3U: {
        if (prevRow == nullptr) {
          for (std::size_t x = 0U; x < prefixBytes; ++x) {
            dstRow[x] = srcRow[x];
          }
          for (std::size_t x = prefixBytes; x < rowBytes; ++x) {
            dstRow[x] = static_cast<std::uint8_t>(
                srcRow[x] + static_cast<std::uint8_t>(static_cast<unsigned int>(dstRow[x - bytesPerPixel]) / 2U));
          }
        } else {
          for (std::size_t x = 0U; x < prefixBytes; ++x) {
            dstRow[x] = static_cast<std::uint8_t>(
                srcRow[x] + static_cast<std::uint8_t>(static_cast<unsigned int>(prevRow[x]) / 2U));
          }
          for (std::size_t x = prefixBytes; x < rowBytes; ++x) {
            dstRow[x] = static_cast<std::uint8_t>(
                srcRow[x] +
                static_cast<std::uint8_t>((static_cast<unsigned int>(dstRow[x - bytesPerPixel]) +
                                           static_cast<unsigned int>(prevRow[x])) /
                                          2U));
          }
        }
        break;
      }
      case 4U: {
        if (prevRow == nullptr) {
          for (std::size_t x = 0U; x < prefixBytes; ++x) {
            dstRow[x] = srcRow[x];
          }
          for (std::size_t x = prefixBytes; x < rowBytes; ++x) {
            dstRow[x] = static_cast<std::uint8_t>(srcRow[x] + dstRow[x - bytesPerPixel]);
          }
        } else {
          for (std::size_t x = 0U; x < prefixBytes; ++x) {
            dstRow[x] = static_cast<std::uint8_t>(srcRow[x] + pathPredictor(0U, prevRow[x], 0U));
          }
          for (std::size_t x = prefixBytes; x < rowBytes; ++x) {
            dstRow[x] = static_cast<std::uint8_t>(
                srcRow[x] + pathPredictor(dstRow[x - bytesPerPixel], prevRow[x], prevRow[x - bytesPerPixel]));
          }
        }
        break;
      }
      default:
        error = "png filter type unsupported";
        return false;
    }

    srcOffset += rowBytes;
  }

  outImage.width = width;
  outImage.height = height;

  if (channels == 4) {
    outImage.rgba = std::move(unfiltered);
    return true;
  }

  outImage.rgba.resize(pixelCount * 4U);

  const std::uint8_t* src = unfiltered.data();
  std::uint8_t* dst = outImage.rgba.data();
  for (std::size_t px = 0U; px < pixelCount; ++px) {
    dst[0U] = src[0U];
    dst[1U] = src[1U];
    dst[2U] = src[2U];
    dst[3U] = 255U;
    src += 3U;
    dst += 4U;
  }

  return true;
}

namespace {

void appendPngChunk(std::vector<std::uint8_t>& png,
                    std::string_view type,
                    const std::vector<std::uint8_t>& data) {
  appendU32Be(png, static_cast<std::uint32_t>(data.size()));

  const std::size_t typeOffset = png.size();
  png.insert(png.end(), type.begin(), type.end());
  png.insert(png.end(), data.begin(), data.end());

  const std::uint32_t crc = crc32(png.data() + typeOffset, type.size() + data.size());
  appendU32Be(png, crc);
}

}  // namespace

bool encodePngRgbaFile(const std::filesystem::path& path, const RawImage& image) {
  if (image.width == 0U || image.height == 0U) {
    return false;
  }

  if (image.height != 0U && image.width > std::numeric_limits<std::size_t>::max() / image.height) {
    return false;
  }
  const std::size_t pixelCount = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
  if (pixelCount > std::numeric_limits<std::size_t>::max() / 4U) {
    return false;
  }
  if (image.rgba.size() < pixelCount * 4U) {
    return false;
  }

  const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4U;
  if (rowBytes > std::numeric_limits<std::size_t>::max() - 1U) {
    return false;
  }
  const std::size_t rowWithFilter = rowBytes + 1U;
  if (image.height != 0U && rowWithFilter > std::numeric_limits<std::size_t>::max() / image.height) {
    return false;
  }
  const std::size_t scanlineBytes = rowWithFilter * static_cast<std::size_t>(image.height);
  constexpr std::size_t kBytesPerPixel = 4U;

  std::vector<std::uint8_t> scanlines;
  scanlines.resize(scanlineBytes, 0U);
  const std::uint8_t* src = image.rgba.data();

  std::vector<std::uint8_t> candidate(rowBytes, 0U);
  std::vector<std::uint8_t> best(rowBytes, 0U);

  for (std::uint32_t y = 0; y < image.height; ++y) {
    const std::uint8_t* row = src + static_cast<std::size_t>(y) * rowBytes;
    const std::uint8_t* prevRow = y > 0U ? row - rowBytes : nullptr;

    std::uint64_t bestScore = 0U;
    std::uint8_t bestFilter = 0U;

    for (std::size_t x = 0U; x < rowBytes; ++x) {
      candidate[x] = row[x];
      bestScore += filterByteCost(candidate[x]);
    }
    bestFilter = 0U;
    best.swap(candidate);

    std::uint64_t score = 0U;
    for (std::size_t x = 0U; x < rowBytes; ++x) {
      const std::uint8_t left = x >= kBytesPerPixel ? row[x - kBytesPerPixel] : 0U;
      candidate[x] = static_cast<std::uint8_t>(row[x] - left);
      score += filterByteCost(candidate[x]);
    }
    if (score < bestScore) {
      bestScore = score;
      bestFilter = 1U;
      best.swap(candidate);
    }

    score = 0U;
    for (std::size_t x = 0U; x < rowBytes; ++x) {
      const std::uint8_t up = prevRow != nullptr ? prevRow[x] : 0U;
      candidate[x] = static_cast<std::uint8_t>(row[x] - up);
      score += filterByteCost(candidate[x]);
    }
    if (score < bestScore) {
      bestScore = score;
      bestFilter = 2U;
      best.swap(candidate);
    }

    score = 0U;
    for (std::size_t x = 0U; x < rowBytes; ++x) {
      const std::uint8_t left = x >= kBytesPerPixel ? row[x - kBytesPerPixel] : 0U;
      const std::uint8_t up = prevRow != nullptr ? prevRow[x] : 0U;
      const std::uint8_t predictor =
          static_cast<std::uint8_t>((static_cast<unsigned int>(left) + static_cast<unsigned int>(up)) / 2U);
      candidate[x] = static_cast<std::uint8_t>(row[x] - predictor);
      score += filterByteCost(candidate[x]);
    }
    if (score < bestScore) {
      bestScore = score;
      bestFilter = 3U;
      best.swap(candidate);
    }

    score = 0U;
    for (std::size_t x = 0U; x < rowBytes; ++x) {
      const std::uint8_t left = x >= kBytesPerPixel ? row[x - kBytesPerPixel] : 0U;
      const std::uint8_t up = prevRow != nullptr ? prevRow[x] : 0U;
      const std::uint8_t upLeft =
          (prevRow != nullptr && x >= kBytesPerPixel) ? prevRow[x - kBytesPerPixel] : 0U;
      candidate[x] = static_cast<std::uint8_t>(row[x] - pathPredictor(left, up, upLeft));
      score += filterByteCost(candidate[x]);
    }
    if (score < bestScore) {
      bestFilter = 4U;
      best.swap(candidate);
    }

    const std::size_t dstRowOffset = static_cast<std::size_t>(y) * rowWithFilter;
    scanlines[dstRowOffset] = bestFilter;
    std::memcpy(scanlines.data() + dstRowOffset + 1U, best.data(), rowBytes);
  }

  std::vector<std::uint8_t> compressed;
  const CompressionResult compression = compressZlibData(scanlines, compressed, 2);
  if (!compression.success) {
    return false;
  }

  std::vector<std::uint8_t> png;
  png.reserve(8U + 25U + compressed.size() + 32U);

  constexpr std::array<std::uint8_t, 8> kPngSignature = {
      0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
  };
  png.insert(png.end(), kPngSignature.begin(), kPngSignature.end());

  std::vector<std::uint8_t> ihdr;
  ihdr.reserve(13U);
  appendU32Be(ihdr, image.width);
  appendU32Be(ihdr, image.height);
  ihdr.push_back(8U);
  ihdr.push_back(6U);
  ihdr.push_back(0U);
  ihdr.push_back(0U);
  ihdr.push_back(0U);

  appendPngChunk(png, "IHDR", ihdr);
  appendPngChunk(png, "IDAT", compressed);
  appendPngChunk(png, "IEND", {});

  return writeBinaryFile(path, png);
}

}  // namespace volt::io::codec
