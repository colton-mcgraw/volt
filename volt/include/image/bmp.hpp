#pragma once

#include <image/image.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>

// -------------------------------------------------------------------------
// bmp
// -------------------------------------------------------------------------
//
// Reading Windows bitmaps: the OS/2 core header and the Windows headers up
// to version 5; 1, 4 and 8 bits per pixel through a palette (uncompressed or
// RLE), and 16, 24 and 32 bits per pixel, plain or with bit-field masks.
// Bottom-up and top-down rows. The result is rgba8 when the file has alpha
// (an alpha mask, or plain 32-bit pixels, whose spare byte many writers use
// for it) that is not zero everywhere, and for RLE files, which can leave
// pixels unwritten; rgb8 otherwise. sRGB-encoded either way: embedded
// profiles are ignored.

namespace volt::image::bmp
{

    bool is_bmp(std::span<const std::uint8_t> data) noexcept;

    std::expected<image, std::string> decode(std::span<const std::uint8_t> data);

} // namespace volt::image::bmp
