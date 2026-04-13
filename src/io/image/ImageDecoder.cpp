#include "volt/io/image/ImageDecoder.hpp"

#include "JpegCodec.hpp"

#include "volt/core/Logging.hpp"

#include "BmpCodec.hpp"
#include "CodecShared.hpp"
#include "PngCodec.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace volt::io {

bool decodeImageFile(const std::filesystem::path& path, RawImage& outImage) {
  outImage = {};

  std::vector<std::uint8_t> bytes;
  if (!codec::readBinaryFile(path, bytes)) {
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "Failed to read image file: ",
        path.string());
    return false;
  }

  std::string error;
  if (codec::hasPngSignature(bytes)) {
    if (codec::decodePng(bytes, outImage, error)) {
      return true;
    }

    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "PNG decode failed for ",
        path.string(),
        ": ",
        error);
    return false;
  }

  if (codec::hasBmpSignature(bytes)) {
    if (codec::decodeBmp(bytes, outImage, error)) {
      return true;
    }

    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "BMP decode failed for ",
        path.string(),
        ": ",
        error);
    return false;
  }

  if (codec::hasJpegSignature(bytes)) {
    if (decodeJpegFile(bytes, outImage, error)) {
      return true;
    }

    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "JPEG decode failed for ",
        path.string(),
        ": ",
        error);
    return false;
  }

  VOLT_LOG_WARN_CAT(
      volt::core::logging::Category::kIO,
      "Unsupported image format for file: ",
      path.string());
  return false;
}

}  // namespace volt::io
