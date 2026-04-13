#include "volt/io/image/ImageEncoder.hpp"

#include "JpegCodec.hpp"

#include "volt/core/Logging.hpp"

#include "BmpCodec.hpp"
#include "PngCodec.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace volt::io {

bool encodeImageFile(const std::filesystem::path& path,
                     const RawImage& image,
                     ImageEncodeFormat format,
                     int jpegQuality) {
  if (image.width == 0 || image.height == 0 || image.rgba.empty()) {
    return false;
  }

  const std::size_t requiredBytes =
      static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4U;
  if (image.rgba.size() < requiredBytes) {
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  switch (format) {
    case ImageEncodeFormat::kPng:
      return codec::encodePngRgbaFile(path, image);
    case ImageEncodeFormat::kJpeg: {
      std::string error;
      if (encodeJpegRgbaFile(path, image, jpegQuality)) {
        return true;
      }

      error = lastJpegCodecError();
      if (error.empty()) {
        error = "jpeg encode failed";
      }

      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "JPEG encode failed for ",
          path.string(),
          ": ",
          error);
      return false;
    }
    case ImageEncodeFormat::kBmp:
      return codec::encodeBmpRgbaFile(path, image);
    default:
      return false;
  }
}

}  // namespace volt::io
