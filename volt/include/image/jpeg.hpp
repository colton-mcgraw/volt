#pragma once

#include <image/image.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// jpeg
// -------------------------------------------------------------------------
//
// Decoding reads 8-bit baseline, extended sequential and progressive
// Huffman-coded JPEG: grayscale, YCbCr, RGB (Adobe transform 0, or
// components named R, G, B), and CMYK or YCCK as Adobe writes them (APP14),
// with any integral chroma subsampling (4:4:4, 4:2:2, 4:2:0, 4:4:0, 4:1:1,
// ...) and restart intervals. Chroma is upsampled by linear interpolation
// between co-sited samples (what libjpeg calls fancy upsampling), and the
// inverse DCT is computed in floating point. The result is gray8 or rgb8,
// sRGB-encoded. The EXIF orientation is applied unless told otherwise.
//
// Not supported, each with an error that says so: arithmetic coding,
// lossless and hierarchical JPEG, 12-bit samples, and heights defined by a
// DNL marker.
//
// A scan that ends early (a truncated download) gives the part of the
// picture that arrived, the rest gray, as libjpeg does; damage that leaves
// no frame or no scan at all is an error.
//
// Encoding writes baseline JFIF: grayscale from gray images, YCbCr from
// everything else, at a quality from 1 to 100 (the IJG scale of the
// standard's example tables), with chroma at full resolution or halved in
// one or both directions, and Huffman tables either the standard's or computed
// for the image (smaller files, a second pass). Alpha is dropped; an image
// with another encoding is re-encoded to sRGB first.

namespace volt::image::jpeg
{

    bool is_jpeg(std::span<const std::uint8_t> data) noexcept;

    struct decode_options
    {
        // Turn the picture upright as its EXIF orientation tag says.
        bool apply_orientation = true;
    };

    std::expected<image, std::string> decode(std::span<const std::uint8_t> data, const decode_options &options = {});

    enum class chroma_subsampling : std::uint8_t
    {
        // 4:4:4, chroma at full resolution.
        none,

        // 4:2:0, chroma halved in both directions: the usual choice for
        // photographs.
        half,

        // 4:2:2, chroma halved horizontally.
        horizontal,

        // 4:4:0, chroma halved vertically.
        vertical,
    };

    struct encode_options
    {
        // 1 (smallest, worst) to 100 (largest, best). 90 is visually
        // lossless for most pictures.
        int quality = 90;
        chroma_subsampling subsampling = chroma_subsampling::half;

        // Huffman tables computed for the image instead of the standard's.
        bool optimize_huffman = true;
    };

    std::expected<std::vector<std::uint8_t>, std::string> encode(const image &source, const encode_options &options = {});

} // namespace volt::image::jpeg
