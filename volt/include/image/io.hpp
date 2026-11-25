#pragma once

#include <image/image.hpp>
#include <image/jpeg.hpp>
#include <image/png.hpp>
#include <image/svg.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// io
// -------------------------------------------------------------------------
//
// The front door: images from files or bytes in whatever format they are,
// and back out again. Loading goes by the content, never the file name (a
// .png that is really a JPEG still loads); saving goes by the extension.
// Errors from a path start with the file's name and say what is wrong with
// the file.

namespace volt::image
{

    enum class file_format : std::uint8_t
    {
        unknown,
        png,
        jpeg,
        bmp,
        svg,
    };

    const char *name(file_format format) noexcept;

    // By content: signatures for PNG, JPEG and BMP, an <svg element (or gzip)
    // for SVG.
    file_format detect(std::span<const std::uint8_t> data) noexcept;

    // By extension, any case: .png; .jpg, .jpeg, .jpe, .jfif; .bmp, .dib;
    // .svg, .svgz.
    file_format format_from_extension(const std::filesystem::path &path) noexcept;

    struct load_options
    {
        jpeg::decode_options jpeg;

        // SVG is drawn at this size (by default its own) into rgba8.
        svg::rasterize_options svg;
    };

    std::expected<image, std::string> load(std::span<const std::uint8_t> data, const load_options &options = {});
    std::expected<image, std::string> load(const std::filesystem::path &path, const load_options &options = {});

    struct save_options
    {
        png::encode_options png;
        jpeg::encode_options jpeg;
    };

    // PNG or JPEG. BMP and SVG are read, not written.
    std::expected<std::vector<std::uint8_t>, std::string> encode(const image &source, file_format format,
                                                                 const save_options &options = {});

    // In the format the extension names.
    std::expected<void, std::string> save(const std::filesystem::path &path, const image &source,
                                          const save_options &options = {});

    // ---- files ------------------------------------------------------------

    // The whole file. Files larger than `limit` bytes are refused.
    std::expected<std::vector<std::uint8_t>, std::string> read_file(const std::filesystem::path &path,
                                                                    std::uintmax_t limit = std::uintmax_t{1} << 30);

    // Written whole, through a temporary file renamed into place, so a
    // failure leaves any previous file as it was.
    std::expected<void, std::string> write_file(const std::filesystem::path &path, std::span<const std::uint8_t> data);

} // namespace volt::image
