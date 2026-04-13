#pragma once

#include "volt/io/image/ImageTypes.hpp"
#include "CodecShared.hpp"

#include <filesystem>
#include <string>

namespace volt::io
{

    [[nodiscard]] const std::string& lastJpegCodecError();

    [[nodiscard]] bool decodeJpegFile(const std::vector<std::uint8_t> &bytes,
                                      RawImage &outImage,
                                      std::string &error);

    [[nodiscard]] bool encodeJpegRgbaFile(
        const std::filesystem::path &path,
        const RawImage &image,
        int jpegQuality);

} // namespace volt::io
