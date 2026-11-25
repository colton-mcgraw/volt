#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// compress
// -------------------------------------------------------------------------
//
// DEFLATE (RFC 1951) and the two wrappers around it that files use: zlib
// (RFC 1950, inside PNG, WOFF, PDF) and gzip (RFC 1952, .svgz, .gz). Part of
// the image library because PNG needs it, in a namespace of its own because
// fonts and models need it as well.
//
// Decompression treats its input as hostile: every read is bounds-checked,
// a truncated or malformed stream is an error that says what is wrong, and
// the output is capped by max_output so that a small file cannot expand
// into all of memory (DEFLATE reaches 1032 : 1).
//
// Compression is LZ77 over a 32 KiB window with hash chains and lazy
// matching, each block written with whichever of dynamic Huffman codes,
// the fixed codes or no compression comes out smallest. Levels follow
// zlib's meaning: 0 stores, 1 is fastest, 9 searches hardest, 6 is the
// usual balance. Output is deterministic for a given input and level.

namespace volt::compress
{

    // ---- checksums ----------------------------------------------------------
    //
    // Both continue a running value: pass the previous result to checksum
    // data that arrives in pieces. The defaults start a new one.

    // CRC-32 as zlib, PNG and gzip compute it (reflected polynomial
    // 0xEDB88320, inverted on entry and exit).
    std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0) noexcept;

    std::uint32_t adler32(std::span<const std::uint8_t> data, std::uint32_t adler = 1) noexcept;

    // ---- DEFLATE ------------------------------------------------------------

    inline constexpr std::size_t default_max_output = std::size_t{1} << 30;

    struct inflate_options
    {
        // Decompressing more than this is an error.
        std::size_t max_output = default_max_output;

        // The expected output size, if known: reserved up front (as far as
        // max_output and the input's size make plausible).
        std::size_t size_hint = 0;
    };

    // A raw DEFLATE stream. Data after the final block is ignored.
    std::expected<std::vector<std::uint8_t>, std::string> inflate(std::span<const std::uint8_t> input,
                                                                  const inflate_options &options = {});

    // `level` is clamped to 0 to 9.
    std::vector<std::uint8_t> deflate(std::span<const std::uint8_t> input, int level = 6);

    // ---- zlib ---------------------------------------------------------------

    // Checks the header and the Adler-32 of the result. Preset dictionaries
    // are refused (no format Volt reads uses them).
    std::expected<std::vector<std::uint8_t>, std::string> zlib_decompress(std::span<const std::uint8_t> input,
                                                                          const inflate_options &options = {});

    std::vector<std::uint8_t> zlib_compress(std::span<const std::uint8_t> input, int level = 6);

    // ---- gzip ---------------------------------------------------------------

    // Whether the data starts like a gzip file.
    bool is_gzip(std::span<const std::uint8_t> input) noexcept;

    // Every member of the file, concatenated (as gunzip does), each checked
    // against its CRC-32 and length.
    std::expected<std::vector<std::uint8_t>, std::string> gzip_decompress(std::span<const std::uint8_t> input,
                                                                          const inflate_options &options = {});

    std::vector<std::uint8_t> gzip_compress(std::span<const std::uint8_t> input, int level = 6);

} // namespace volt::compress
