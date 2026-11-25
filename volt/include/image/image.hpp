#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// image
// -------------------------------------------------------------------------
//
// Pixels in memory, as the codecs produce and consume them: a width, a
// height, a pixel format, and the rows one after the other, top row first,
// with no padding between them. Nothing here knows about the GPU; the layout
// is simply the one a texture upload wants (contiguous, tightly packed, so
// stride() == width * pixel_size), and rgba8 with an sRGB encoding is what
// to_rgba8_srgb() hands the renderer.
//
// Channels are unsigned normalized integers: 0 is 0.0 and the largest value
// (255 or 65535) is 1.0. 16-bit channels are std::uint16_t in the machine's
// byte order (not PNG's big-endian), so a 16-bit image uploads as-is to an
// R16G16B16A16_UNORM-style format. Read and write them with load16/store16,
// which go through memcpy and so stay clear of aliasing rules.
//
// What the numbers mean is recorded alongside them: the transfer curve they
// are encoded with (see color_encoding), and whether color is premultiplied
// by alpha. The primaries are always taken to be sRGB's (BT.709), the
// working space of <gpu/color.hpp>; files tagged otherwise are not
// converted.

namespace volt::image
{

    enum class pixel_format : std::uint8_t
    {
        gray8,
        gray_alpha8,
        rgb8,
        rgba8,
        gray16,
        gray_alpha16,
        rgb16,
        rgba16,
    };

    constexpr std::uint32_t channel_count(pixel_format format) noexcept
    {
        switch (format)
        {
        case pixel_format::gray8:
        case pixel_format::gray16:
            return 1;
        case pixel_format::gray_alpha8:
        case pixel_format::gray_alpha16:
            return 2;
        case pixel_format::rgb8:
        case pixel_format::rgb16:
            return 3;
        case pixel_format::rgba8:
        case pixel_format::rgba16:
            return 4;
        }
        return 0;
    }

    // Bytes per channel: 1 or 2.
    constexpr std::uint32_t channel_size(pixel_format format) noexcept
    {
        return format >= pixel_format::gray16 ? 2u : 1u;
    }

    constexpr std::uint32_t pixel_size(pixel_format format) noexcept
    {
        return channel_count(format) * channel_size(format);
    }

    constexpr bool has_alpha(pixel_format format) noexcept
    {
        return channel_count(format) == 2 || channel_count(format) == 4;
    }

    constexpr bool is_gray(pixel_format format) noexcept
    {
        return channel_count(format) <= 2;
    }

    // The format with the same channels at the other depth.
    constexpr pixel_format with_depth(pixel_format format, bool sixteen) noexcept
    {
        const auto index = static_cast<std::uint8_t>(static_cast<std::uint8_t>(format) % 4u + (sixteen ? 4u : 0u));
        return static_cast<pixel_format>(index);
    }

    // The format with these channels at that depth.
    constexpr pixel_format make_format(std::uint32_t channels, bool sixteen) noexcept
    {
        const std::uint32_t index = (channels >= 1 && channels <= 4 ? channels - 1 : 3) + (sixteen ? 4u : 0u);
        return static_cast<pixel_format>(index);
    }

    const char *name(pixel_format format) noexcept;

    // ---- color encoding ---------------------------------------------------

    enum class transfer : std::uint8_t
    {
        // The sRGB curve. What nearly every image file holds, and what one
        // that says nothing is assumed to hold.
        srgb,

        // Values are proportional to light.
        linear,

        // A pure power law: linear = encoded ^ gamma. What a PNG gAMA chunk
        // describes when it is not one of the two above.
        gamma,
    };

    struct color_encoding
    {
        transfer curve = transfer::srgb;

        // The exponent for transfer::gamma, ignored otherwise. 2.2 is the
        // classic display gamma; PNG's gAMA chunk stores its reciprocal.
        float gamma = 2.2f;

        friend bool operator==(const color_encoding &, const color_encoding &) = default;
    };

    inline constexpr color_encoding srgb_encoding{transfer::srgb, 2.2f};
    inline constexpr color_encoding linear_encoding{transfer::linear, 1.0f};

    // One channel value in 0 to 1 through the curve, either way.
    float to_linear(const color_encoding &encoding, float encoded) noexcept;
    float from_linear(const color_encoding &encoding, float linear) noexcept;

    // ---- limits -------------------------------------------------------------
    //
    // What a decoder accepts before it allocates anything. Generous for real
    // images, small enough that a hostile header cannot ask for more memory
    // than a desktop has: 2^27 pixels is 512 MiB as rgba8.

    inline constexpr std::uint32_t max_dimension = 65535;
    inline constexpr std::uint64_t max_pixels = std::uint64_t{1} << 27;

    // Whether an image of that size may be created; the reason if not.
    std::expected<void, std::string> check_size(std::uint64_t width, std::uint64_t height);

    // ---- 16-bit access ------------------------------------------------------

    inline std::uint16_t load16(const std::uint8_t *at) noexcept
    {
        std::uint16_t value = 0;
        std::memcpy(&value, at, sizeof value);
        return value;
    }

    inline void store16(std::uint8_t *at, std::uint16_t value) noexcept
    {
        std::memcpy(at, &value, sizeof value);
    }

    // ---------------------------------------------------------------------
    // image
    // ---------------------------------------------------------------------
    //
    // Owns its pixels. Copyable (a deep copy) and movable; an image that was
    // default-constructed or moved from is empty.
    class image
    {
    public:
        image() = default;

        // A zeroed image (transparent black where there is alpha). Fails only
        // for sizes check_size() rejects, or a zero width or height.
        static std::expected<image, std::string> create(std::uint32_t width, std::uint32_t height,
                                                        pixel_format format,
                                                        color_encoding encoding = srgb_encoding);

        bool empty() const noexcept { return m_pixels.empty(); }
        std::uint32_t width() const noexcept { return m_width; }
        std::uint32_t height() const noexcept { return m_height; }
        pixel_format format() const noexcept { return m_format; }

        // Bytes from the start of one row to the start of the next. Always
        // width * pixel_size(format): rows are tightly packed.
        std::size_t stride() const noexcept { return std::size_t{m_width} * pixel_size(m_format); }
        std::size_t size_bytes() const noexcept { return m_pixels.size(); }

        std::uint8_t *data() noexcept { return m_pixels.data(); }
        const std::uint8_t *data() const noexcept { return m_pixels.data(); }
        std::span<std::uint8_t> pixels() noexcept { return m_pixels; }
        std::span<const std::uint8_t> pixels() const noexcept { return m_pixels; }

        // Row y, 0 at the top. y must be below height().
        std::span<std::uint8_t> row(std::uint32_t y) noexcept { return {m_pixels.data() + y * stride(), stride()}; }
        std::span<const std::uint8_t> row(std::uint32_t y) const noexcept
        {
            return {m_pixels.data() + y * stride(), stride()};
        }

        // The pixel at (x, y): pixel_size(format()) bytes.
        std::uint8_t *at(std::uint32_t x, std::uint32_t y) noexcept
        {
            return m_pixels.data() + y * stride() + std::size_t{x} * pixel_size(m_format);
        }
        const std::uint8_t *at(std::uint32_t x, std::uint32_t y) const noexcept
        {
            return m_pixels.data() + y * stride() + std::size_t{x} * pixel_size(m_format);
        }

        // How the channel values map to light. Setting it relabels the
        // values; reencode() converts them.
        const color_encoding &encoding() const noexcept { return m_encoding; }
        void set_encoding(const color_encoding &encoding) noexcept { m_encoding = encoding; }

        // Whether color has been multiplied by alpha. Decoders produce
        // straight alpha, as every file format stores it; premultiply()
        // and unpremultiply() convert and keep this up to date.
        bool premultiplied() const noexcept { return m_premultiplied; }
        void set_premultiplied(bool premultiplied) noexcept { m_premultiplied = premultiplied; }

    private:
        std::uint32_t m_width = 0;
        std::uint32_t m_height = 0;
        pixel_format m_format = pixel_format::rgba8;
        color_encoding m_encoding = srgb_encoding;
        bool m_premultiplied = false;
        std::vector<std::uint8_t> m_pixels;
    };

    // ---- conversions ------------------------------------------------------
    //
    // None of these can fail on a valid image; an empty image gives an empty
    // one back.

    // The same pixels in another layout. Values are rescaled between 8 and 16
    // bits exactly (v * 257 one way, rounding the other); gray becomes RGB by
    // repetition and RGB becomes gray by its BT.709 luma, computed on the
    // stored values; a missing alpha is opaque and a dropped one is simply
    // discarded (nothing is composited). The encoding is unchanged.
    image convert(const image &source, pixel_format format);

    // The same picture with its values re-encoded through another transfer
    // curve. Keeps the format, and the alpha mode.
    image reencode(const image &source, const color_encoding &encoding);

    // What a texture is made from: rgba8, sRGB-encoded, straight alpha. A
    // copy (conversions skipped where they are not needed).
    image to_rgba8_srgb(const image &source);

    // Multiply color by alpha, or divide it back out. The multiplication is
    // done in linear light: an sRGB-encoded channel c becomes
    // encode(decode(c) * alpha), which is what a texture of an sRGB format
    // must hold for blending in linear light to be right. No effect without
    // alpha, or when already in the requested state.
    void premultiply(image &target);
    void unpremultiply(image &target);

    // ---- scaling ----------------------------------------------------------
    //
    // A box filter, done in linear light and weighted by alpha, so that
    // shrinking neither darkens the picture nor lets the color of fully
    // transparent pixels bleed in. Each destination pixel is the area-weighted
    // average of the source pixels it covers; enlarging therefore gives
    // blocky (nearest-like) results and is not what this is for. The result
    // has the source's format, encoding and alpha mode.

    image resize(const image &source, std::uint32_t width, std::uint32_t height);

    // Half the size in each dimension, rounded down but at least 1: the next
    // mip level.
    image downsample(const image &source);

    // The source followed by successive downsample()s, down to 1 x 1, or at
    // most `levels` images in all when levels is nonzero.
    std::vector<image> mip_chain(const image &source, std::uint32_t levels = 0);

    // ---- orientation ------------------------------------------------------

    // Mirror top to bottom, in place.
    void flip_vertical(image &target);

    // The eight EXIF orientations (1 = as stored, 2..8 the mirrored and
    // rotated ones, as TIFF tag 0x0112 numbers them): the stored image turned
    // so it displays upright. Values outside 1 to 8 return a copy.
    image apply_orientation(const image &source, int orientation);

} // namespace volt::image
