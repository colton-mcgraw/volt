#include "volt/io/ImageCodec.hpp"

#include "volt/core/Logging.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

namespace volt::io {

bool decodeImageFile(const std::filesystem::path& path, DecodedImage& outImage) {
  const std::string file = path.string();

  int width = 0;
  int height = 0;
  int channels = 0;
  stbi_uc* pixels = stbi_load(file.c_str(), &width, &height, &channels, 4);
  if (pixels == nullptr) {
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "stb decode failed for ",
        file,
        ": ",
        stbi_failure_reason() == nullptr ? "unknown error" : stbi_failure_reason());
    return false;
  }

  outImage.width = static_cast<std::uint32_t>(width);
  outImage.height = static_cast<std::uint32_t>(height);
  outImage.rgba.assign(
      pixels,
      pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);

  stbi_image_free(pixels);
  return true;
}

bool encodeImageFile(const std::filesystem::path& path,
                     const DecodedImage& image,
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

  const std::string file = path.string();
  const int width = static_cast<int>(image.width);
  const int height = static_cast<int>(image.height);
  const int stride = width * 4;

  int ok = 0;
  switch (format) {
    case ImageEncodeFormat::kPng:
      ok = stbi_write_png(file.c_str(), width, height, 4, image.rgba.data(), stride);
      break;
    case ImageEncodeFormat::kJpeg:
      jpegQuality = std::clamp(jpegQuality, 1, 100);
      ok = stbi_write_jpg(file.c_str(), width, height, 4, image.rgba.data(), jpegQuality);
      break;
    case ImageEncodeFormat::kBmp:
      ok = stbi_write_bmp(file.c_str(), width, height, 4, image.rgba.data());
      break;
    default:
      return false;
  }

  if (ok == 0) {
    VOLT_LOG_WARN_CAT(volt::core::logging::Category::kIO, "stb encode failed for ", file);
    return false;
  }

  return true;
}

}  // namespace volt::io
