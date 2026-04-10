#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace volt::io {

enum class ImageEncodeFormat {
  kPng,
  kJpeg,
  kBmp,
};

struct DecodedImage {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::vector<std::uint8_t> rgba;
};

[[nodiscard]] bool decodeImageFile(const std::filesystem::path& path, DecodedImage& outImage);
[[nodiscard]] bool encodeImageFile(const std::filesystem::path& path,
                                   const DecodedImage& image,
                                   ImageEncodeFormat format,
                                   int jpegQuality = 90);

}  // namespace volt::io
