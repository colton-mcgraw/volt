#pragma once

#include <image/image.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// png
// -------------------------------------------------------------------------
//
// Decoding reads every PNG the specification allows: gray, gray with alpha,
// RGB, RGBA and palette images at every bit depth (1, 2, 4, 8, 16), Adam7
// interlacing, all five row filters, and transparency from tRNS. Every
// chunk's CRC is checked. The result is gray8, gray_alpha8, rgb8 or rgba8,
// or the 16-bit format with the same channels for 16-bit files; palette
// images become rgb8 (rgba8 with tRNS), lower depths are scaled up to 8
// bits, and a tRNS color key turns into an alpha channel.
//
// The encoding comes from the sRGB and gAMA chunks: sRGB, or a gAMA of
// 1/2.2, gives the sRGB curve; a gAMA of 1 linear; anything else
// transfer::gamma. A file with neither is taken to be sRGB, as browsers do.
// Color profiles (iCCP) and chromaticities (cHRM) are not applied.
//
// Encoding writes 8- and 16-bit gray, gray with alpha, RGB and RGBA, as the
// image's format says, with an sRGB or gAMA chunk for its encoding.
// Premultiplied images are unpremultiplied first, since PNG stores straight
// alpha.

namespace volt::image::png
{

    // Whether the data starts with the PNG signature.
    bool is_png(std::span<const std::uint8_t> data) noexcept;

    std::expected<image, std::string> decode(std::span<const std::uint8_t> data);

    enum class filter_strategy : std::uint8_t
    {
        // Per row, the filter whose output has the smallest sum of absolute
        // values (as signed bytes): the usual heuristic, and the default.
        adaptive,

        // No filtering: fastest, and best for palette-like images.
        none,
    };

    struct encode_options
    {
        // zlib level, 0 (store) to 9.
        int compression = 6;
        filter_strategy filter = filter_strategy::adaptive;
    };

    std::expected<std::vector<std::uint8_t>, std::string> encode(const image &source, const encode_options &options = {});

} // namespace volt::image::png
