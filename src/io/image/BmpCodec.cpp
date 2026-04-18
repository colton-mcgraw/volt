#include "BmpCodec.hpp"

#include "CodecShared.hpp"

#include <limits>

namespace {

constexpr std::size_t kBmpFileHeaderBytes = 14U;
constexpr std::size_t kBmpInfoHeaderBytes = 40U;
constexpr std::size_t kBmpPixelDataOffset = kBmpFileHeaderBytes + kBmpInfoHeaderBytes;
constexpr std::size_t kMaxDecodedImageBytes = 256U * 1024U * 1024U;
constexpr std::uint32_t kBmpCompressionRgb = 0U;
constexpr std::uint32_t kBmpCompressionBitfields = 3U;

void writeU16LeAt(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value) {
  out[offset + 0U] = static_cast<std::uint8_t>(value & 0xFFU);
  out[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void writeU32LeAt(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
  out[offset + 0U] = static_cast<std::uint8_t>(value & 0xFFU);
  out[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  out[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  out[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

std::uint8_t decodeMaskedChannel(std::uint32_t packedPixel, std::uint32_t mask) {
  if (mask == 0U) {
    return 255U;
  }

  std::uint32_t shift = 0U;
  while (((mask >> shift) & 1U) == 0U && shift < 31U) {
    ++shift;
  }

  const std::uint32_t maskBits = mask >> shift;
  if (maskBits == 0U) {
    return 0U;
  }

  const std::uint32_t value = (packedPixel & mask) >> shift;
  const std::uint64_t scaled = static_cast<std::uint64_t>(value) * 255U + (maskBits / 2U);
  return static_cast<std::uint8_t>(scaled / maskBits);
}

bool readBitfieldMasks(const std::vector<std::uint8_t>& bytes,
                       std::uint32_t dibHeaderSize,
                       std::uint32_t& redMask,
                       std::uint32_t& greenMask,
                       std::uint32_t& blueMask,
                       std::uint32_t& alphaMask) {
  redMask = 0U;
  greenMask = 0U;
  blueMask = 0U;
  alphaMask = 0U;

  if (dibHeaderSize >= 56U) {
    const std::size_t maskOffset = kBmpFileHeaderBytes + 40U;
    return volt::io::codec::readU32Le(bytes, maskOffset + 0U, redMask) &&
           volt::io::codec::readU32Le(bytes, maskOffset + 4U, greenMask) &&
           volt::io::codec::readU32Le(bytes, maskOffset + 8U, blueMask) &&
           volt::io::codec::readU32Le(bytes, maskOffset + 12U, alphaMask);
  }

  if (dibHeaderSize < 40U) {
    return false;
  }

  const std::size_t maskOffset = kBmpFileHeaderBytes + static_cast<std::size_t>(dibHeaderSize);
  if (!volt::io::codec::readU32Le(bytes, maskOffset + 0U, redMask) ||
      !volt::io::codec::readU32Le(bytes, maskOffset + 4U, greenMask) ||
      !volt::io::codec::readU32Le(bytes, maskOffset + 8U, blueMask)) {
    return false;
  }

  volt::io::codec::readU32Le(bytes, maskOffset + 12U, alphaMask);
  return true;
}

}  // namespace

namespace volt::io::codec {

bool decodeBmp(const std::vector<std::uint8_t>& bytes, RawImage& outImage, std::string& error) {
  if (bytes.size() < 54U) {
    error = "bmp file too small";
    return false;
  }

  if (!hasBmpSignature(bytes)) {
    error = "bmp signature missing";
    return false;
  }

  std::uint32_t pixelDataOffset = 0U;
  std::uint32_t dibHeaderSize = 0U;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::uint16_t planes = 0U;
  std::uint16_t bitsPerPixel = 0U;
  std::uint32_t compression = 0U;

  if (!readU32Le(bytes, 10U, pixelDataOffset) ||
      !readU32Le(bytes, 14U, dibHeaderSize) ||
      !readI32Le(bytes, 18U, width) ||
      !readI32Le(bytes, 22U, height) ||
      !readU16Le(bytes, 26U, planes) ||
      !readU16Le(bytes, 28U, bitsPerPixel) ||
      !readU32Le(bytes, 30U, compression)) {
    error = "bmp header truncated";
    return false;
  }

  if (dibHeaderSize < 40U || planes != 1U) {
    error = "bmp header format unsupported";
    return false;
  }

  if (compression != kBmpCompressionRgb && compression != kBmpCompressionBitfields) {
    error = "bmp compression unsupported";
    return false;
  }

  if (width <= 0 || height == 0) {
    error = "bmp dimensions invalid";
    return false;
  }

  if (height == std::numeric_limits<std::int32_t>::min()) {
    error = "bmp dimensions invalid";
    return false;
  }

  if (bitsPerPixel != 24U && bitsPerPixel != 32U) {
    error = "bmp bit depth unsupported";
    return false;
  }

  if (compression == kBmpCompressionBitfields && bitsPerPixel != 32U) {
    error = "bmp bitfields bit depth unsupported";
    return false;
  }

  if (dibHeaderSize > std::numeric_limits<std::size_t>::max() - kBmpFileHeaderBytes) {
    error = "bmp header size overflow";
    return false;
  }

  const std::size_t minimumPixelDataOffset =
      kBmpFileHeaderBytes + static_cast<std::size_t>(dibHeaderSize) +
      ((compression == kBmpCompressionBitfields && dibHeaderSize == kBmpInfoHeaderBytes) ? 12U : 0U);
  if (pixelDataOffset < minimumPixelDataOffset) {
    error = "bmp pixel data offset invalid";
    return false;
  }

  std::uint32_t redMask = 0x00FF0000U;
  std::uint32_t greenMask = 0x0000FF00U;
  std::uint32_t blueMask = 0x000000FFU;
  std::uint32_t alphaMask = 0xFF000000U;
  if (compression == kBmpCompressionBitfields &&
      !readBitfieldMasks(bytes, dibHeaderSize, redMask, greenMask, blueMask, alphaMask)) {
    error = "bmp bitfield masks missing";
    return false;
  }

  const bool topDown = height < 0;
  const std::uint32_t absHeight = static_cast<std::uint32_t>(
      topDown ? -static_cast<std::int64_t>(height) : static_cast<std::int64_t>(height));
  const std::uint32_t absWidth = static_cast<std::uint32_t>(width);

  const std::size_t bytesPerPixel = bitsPerPixel / 8U;
  if (absWidth != 0U && bytesPerPixel > std::numeric_limits<std::size_t>::max() / absWidth) {
    error = "bmp dimensions overflow";
    return false;
  }

  const std::size_t packedRowBytes = static_cast<std::size_t>(absWidth) * bytesPerPixel;
  if (packedRowBytes > std::numeric_limits<std::size_t>::max() - 3U) {
    error = "bmp dimensions overflow";
    return false;
  }

  const std::size_t rowStride = (packedRowBytes + 3U) & ~std::size_t{3U};
  if (absHeight != 0U && rowStride > std::numeric_limits<std::size_t>::max() / absHeight) {
    error = "bmp dimensions overflow";
    return false;
  }

  const std::size_t pixelSpan = rowStride * static_cast<std::size_t>(absHeight);
  if (pixelDataOffset > bytes.size() || pixelSpan > bytes.size() - pixelDataOffset) {
    error = "bmp pixel data truncated";
    return false;
  }

  if (absHeight != 0U && absWidth > std::numeric_limits<std::size_t>::max() / absHeight) {
    error = "bmp dimensions overflow";
    return false;
  }

  const std::size_t pixelCount = static_cast<std::size_t>(absWidth) * static_cast<std::size_t>(absHeight);
  if (pixelCount > kMaxDecodedImageBytes / 4U) {
    error = "bmp dimensions exceed safety limit";
    return false;
  }

  if (pixelCount > std::numeric_limits<std::size_t>::max() / 4U) {
    error = "bmp dimensions overflow";
    return false;
  }

  outImage.width = absWidth;
  outImage.height = absHeight;
  outImage.rgba.resize(pixelCount * 4U);

  const std::uint8_t* bytesData = bytes.data();
  std::uint8_t* outData = outImage.rgba.data();

  if (bitsPerPixel == 32U && compression == kBmpCompressionBitfields) {
    for (std::uint32_t y = 0U; y < absHeight; ++y) {
      const std::uint32_t srcY = topDown ? y : (absHeight - 1U - y);
      const std::uint8_t* src = bytesData + static_cast<std::size_t>(pixelDataOffset) +
                                static_cast<std::size_t>(srcY) * rowStride;
      std::uint8_t* dst = outData + static_cast<std::size_t>(y) * static_cast<std::size_t>(absWidth) * 4U;

      for (std::uint32_t x = 0U; x < absWidth; ++x) {
        const std::uint32_t packedPixel =
            static_cast<std::uint32_t>(src[0U]) |
            (static_cast<std::uint32_t>(src[1U]) << 8U) |
            (static_cast<std::uint32_t>(src[2U]) << 16U) |
            (static_cast<std::uint32_t>(src[3U]) << 24U);
        dst[0U] = decodeMaskedChannel(packedPixel, redMask);
        dst[1U] = decodeMaskedChannel(packedPixel, greenMask);
        dst[2U] = decodeMaskedChannel(packedPixel, blueMask);
        dst[3U] = decodeMaskedChannel(packedPixel, alphaMask);
        src += 4U;
        dst += 4U;
      }
    }

    return true;
  }

  if (bitsPerPixel == 32U) {
    for (std::uint32_t y = 0U; y < absHeight; ++y) {
      const std::uint32_t srcY = topDown ? y : (absHeight - 1U - y);
      const std::uint8_t* src = bytesData + static_cast<std::size_t>(pixelDataOffset) +
                                static_cast<std::size_t>(srcY) * rowStride;
      std::uint8_t* dst = outData + static_cast<std::size_t>(y) * static_cast<std::size_t>(absWidth) * 4U;

      for (std::uint32_t x = 0U; x < absWidth; ++x) {
        dst[0U] = src[2U];
        dst[1U] = src[1U];
        dst[2U] = src[0U];
        dst[3U] = src[3U];
        src += 4U;
        dst += 4U;
      }
    }

    return true;
  }

  for (std::uint32_t y = 0U; y < absHeight; ++y) {
    const std::uint32_t srcY = topDown ? y : (absHeight - 1U - y);
    const std::uint8_t* src = bytesData + static_cast<std::size_t>(pixelDataOffset) +
                              static_cast<std::size_t>(srcY) * rowStride;
    std::uint8_t* dst = outData + static_cast<std::size_t>(y) * static_cast<std::size_t>(absWidth) * 4U;

    for (std::uint32_t x = 0U; x < absWidth; ++x) {
      dst[0U] = src[2U];
      dst[1U] = src[1U];
      dst[2U] = src[0U];
      dst[3U] = 255U;
      src += 3U;
      dst += 4U;
    }
  }

  return true;
}

bool encodeBmpRgbaFile(const std::filesystem::path& path, const RawImage& image) {
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

  const std::size_t rowStride = static_cast<std::size_t>(image.width) * 4U;
  if (rowStride != 0U && image.height > std::numeric_limits<std::size_t>::max() / rowStride) {
    return false;
  }
  const std::size_t pixelBytes = rowStride * static_cast<std::size_t>(image.height);

  const std::size_t totalBytes = kBmpPixelDataOffset + pixelBytes;
  if (totalBytes > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }

  std::vector<std::uint8_t> bmp;
  bmp.resize(totalBytes, 0U);

  bmp[0U] = static_cast<std::uint8_t>('B');
  bmp[1U] = static_cast<std::uint8_t>('M');
  writeU32LeAt(bmp, 2U, static_cast<std::uint32_t>(totalBytes));
  writeU16LeAt(bmp, 6U, 0U);
  writeU16LeAt(bmp, 8U, 0U);
  writeU32LeAt(bmp, 10U, static_cast<std::uint32_t>(kBmpPixelDataOffset));

  writeU32LeAt(bmp, 14U, static_cast<std::uint32_t>(kBmpInfoHeaderBytes));
  writeU32LeAt(bmp, 18U, image.width);
  writeU32LeAt(bmp, 22U, image.height);
  writeU16LeAt(bmp, 26U, 1U);
  writeU16LeAt(bmp, 28U, 32U);
  writeU32LeAt(bmp, 30U, 0U);
  writeU32LeAt(bmp, 34U, static_cast<std::uint32_t>(pixelBytes));
  writeU32LeAt(bmp, 38U, 2835U);
  writeU32LeAt(bmp, 42U, 2835U);
  writeU32LeAt(bmp, 46U, 0U);
  writeU32LeAt(bmp, 50U, 0U);

  std::uint8_t* dst = bmp.data() + kBmpPixelDataOffset;
  const std::uint8_t* srcData = image.rgba.data();

  for (std::uint32_t y = 0; y < image.height; ++y) {
    const std::uint32_t srcY = image.height - 1U - y;
    const std::uint8_t* src = srcData + static_cast<std::size_t>(srcY) * rowStride;
    for (std::uint32_t x = 0; x < image.width; ++x) {
      dst[0U] = src[2U];
      dst[1U] = src[1U];
      dst[2U] = src[0U];
      dst[3U] = src[3U];
      dst += 4U;
      src += 4U;
    }
  }

  return writeBinaryFile(path, bmp);
}

}  // namespace volt::io::codec
