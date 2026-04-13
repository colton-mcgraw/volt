#pragma once

#include "volt/io/image/ImageTypes.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace volt::io::codec {

[[nodiscard]] bool decodeBmp(const std::vector<std::uint8_t>& bytes, RawImage& outImage, std::string& error);
[[nodiscard]] bool encodeBmpRgbaFile(const std::filesystem::path& path, const RawImage& image);

}  // namespace volt::io::codec
