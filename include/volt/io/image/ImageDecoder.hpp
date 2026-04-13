#pragma once

#include "volt/io/image/ImageTypes.hpp"

#include <filesystem>

namespace volt::io {

[[nodiscard]] bool decodeImageFile(const std::filesystem::path& path, RawImage& outImage);

}  // namespace volt::io
