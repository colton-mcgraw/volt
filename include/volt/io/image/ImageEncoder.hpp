#pragma once

#include "volt/io/image/ImageTypes.hpp"

#include <filesystem>

namespace volt::io {

[[nodiscard]] bool encodeImageFile(const std::filesystem::path& path,
                                   const RawImage& image,
                                   ImageEncodeFormat format,
                                   int jpegQuality = 90);

}  // namespace volt::io
