#pragma once

#include <cstdint>
#include <vector>

namespace volt::io {

enum class ImageEncodeFormat {
  kPng,
  kJpeg,
  kBmp,
};

struct RawImage {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::vector<std::uint8_t> rgba;
};

}  // namespace volt::io