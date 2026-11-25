#include <image/bmp.hpp>

#include "bytes.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>

namespace volt::image::bmp
{

    namespace
    {
        using failure = std::unexpected<std::string>;

        enum compression : std::uint32_t
        {
            rgb = 0,
            rle8 = 1,
            rle4 = 2,
            bitfields = 3,
            alpha_bitfields = 6,
        };

        // A channel described by a bit mask: where it is and how wide.
        struct channel
        {
            std::uint32_t mask = 0;
            unsigned shift = 0;
            unsigned bits = 0;

            explicit channel(std::uint32_t m = 0) : mask(m)
            {
                if (m == 0)
                    return;
                shift = static_cast<unsigned>(std::countr_zero(m));
                bits = static_cast<unsigned>(std::popcount(m));
            }

            std::uint8_t extract(std::uint32_t pixel) const
            {
                if (mask == 0)
                    return 0;
                const std::uint32_t value = (pixel & mask) >> shift;
                if (bits >= 8)
                    return static_cast<std::uint8_t>(value >> (bits - 8));
                const std::uint32_t top = (1u << bits) - 1;
                return static_cast<std::uint8_t>((value * 255 + top / 2) / top);
            }
        };
    } // namespace

    bool is_bmp(std::span<const std::uint8_t> data) noexcept
    {
        return data.size() >= 18 && data[0] == 'B' && data[1] == 'M';
    }

    std::expected<image, std::string> decode(std::span<const std::uint8_t> data)
    {
        if (!is_bmp(data))
            return failure("the data is not a BMP file (the signature is missing)");

        detail::byte_reader in(data);
        in.skip(10);
        const std::uint32_t pixel_offset = in.le32();
        const std::uint32_t header_size = in.le32();

        std::int64_t width = 0;
        std::int64_t height = 0;
        std::uint32_t bits = 0;
        std::uint32_t method = rgb;
        std::uint32_t colors_used = 0;
        std::uint32_t masks[4] = {};
        bool core = false;

        if (header_size == 12)
        {
            core = true;
            width = in.le16();
            height = static_cast<std::int16_t>(in.le16());
            in.skip(2);
            bits = in.le16();
        }
        else if (header_size >= 40 && header_size <= 124)
        {
            width = static_cast<std::int32_t>(in.le32());
            height = static_cast<std::int32_t>(in.le32());
            in.skip(2);
            bits = in.le16();
            method = in.le32();
            in.skip(12); // image size, resolution
            colors_used = in.le32();
            in.skip(4);
            if (header_size >= 52)
                for (int i = 0; i < (header_size >= 56 ? 4 : 3); ++i)
                    masks[i] = in.le32();
            in.seek(14 + header_size);
            if (header_size == 40 && (method == bitfields || method == alpha_bitfields))
                for (int i = 0; i < (method == alpha_bitfields ? 4 : 3); ++i)
                    masks[i] = in.le32();
        }
        else
            return failure(std::format("the BMP has an unknown header of {} bytes", header_size));
        if (!in.ok())
            return failure("the BMP file is truncated in its header");

        const bool top_down = height < 0;
        height = top_down ? -height : height;
        if (width <= 0 || height <= 0)
            return failure(std::format("the BMP has an invalid size of {} x {}", width, height));
        if (auto size = check_size(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height)); !size)
            return failure(std::move(size.error()));
        const auto w = static_cast<std::uint32_t>(width);
        const auto h = static_cast<std::uint32_t>(height);

        const bool rle = method == rle8 || method == rle4;
        if (method == rle8 && bits != 8)
            return failure("the BMP is RLE8-compressed but not 8 bits per pixel");
        if (method == rle4 && bits != 4)
            return failure("the BMP is RLE4-compressed but not 4 bits per pixel");
        if (method != rgb && !rle && method != bitfields && method != alpha_bitfields)
            return failure(std::format("the BMP uses compression method {}, which is not supported (JPEG or PNG "
                                       "inside a BMP, or a printer format)",
                                       method));
        if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32)
            return failure(std::format("the BMP has {} bits per pixel, which is not a valid depth", bits));
        if (top_down && rle)
            return failure("the BMP is compressed and top-down, which the format does not allow");

        // The palette follows the header (and its masks).
        std::array<std::array<std::uint8_t, 3>, 256> palette{};
        if (bits <= 8)
        {
            const std::uint32_t entries = colors_used == 0 ? 1u << bits : std::min(colors_used, 1u << bits);
            for (std::uint32_t i = 0; i < entries; ++i)
            {
                const std::uint8_t b = in.u8();
                const std::uint8_t g = in.u8();
                const std::uint8_t r = in.u8();
                if (!core)
                    in.skip(1);
                palette[i] = {r, g, b};
            }
            if (!in.ok())
                return failure("the BMP file is truncated in its palette");
        }

        // Masks for the direct-color depths.
        if (method == rgb && bits == 16)
        {
            masks[0] = 0x7C00;
            masks[1] = 0x03E0;
            masks[2] = 0x001F;
            masks[3] = 0;
        }
        else if (method == rgb && (bits == 24 || bits == 32))
        {
            masks[0] = 0x00FF0000;
            masks[1] = 0x0000FF00;
            masks[2] = 0x000000FF;
            // Plain 32-bit files often put alpha in the spare byte;
            // whether they do is decided once all pixels are read.
            masks[3] = bits == 32 ? 0xFF000000u : 0u;
        }
        if (method == alpha_bitfields && header_size == 40 && bits != 32 && bits != 16)
            return failure("the BMP has bit fields at a depth that cannot hold them");
        const channel channels[4] = {channel(masks[0]), channel(masks[1]), channel(masks[2]), channel(masks[3])};

        auto created = image::create(w, h, pixel_format::rgba8);
        if (!created)
            return std::unexpected(std::move(created.error()));
        image result = std::move(*created);

        if (pixel_offset >= data.size())
            return failure("the BMP file is truncated before its pixels");
        const std::span<const std::uint8_t> pixels = data.subspan(pixel_offset);

        const auto row_of = [&](std::uint32_t stored) { return top_down ? stored : h - 1 - stored; };
        bool any_alpha = false;

        if (rle)
        {
            // Pixels not written by the RLE stream (skipped by a delta) stay
            // transparent black; alpha is kept for that reason.
            detail::byte_reader rl(pixels);
            std::uint32_t x = 0;
            std::uint32_t y = 0;
            const auto put = [&](std::uint32_t index) {
                if (x < w && y < h)
                {
                    std::uint8_t *p = result.at(x, row_of(y));
                    p[0] = palette[index][0];
                    p[1] = palette[index][1];
                    p[2] = palette[index][2];
                    p[3] = 255;
                }
                ++x;
            };
            while (y < h)
            {
                const std::uint8_t count = rl.u8();
                const std::uint8_t value = rl.u8();
                if (!rl.ok())
                    break; // as much as there is
                if (count > 0)
                {
                    for (std::uint32_t i = 0; i < count; ++i)
                        put(method == rle8 ? value : (i % 2 == 0 ? value >> 4 : value & 15u));
                    continue;
                }
                if (value == 0)
                {
                    x = 0;
                    ++y;
                }
                else if (value == 1)
                    break;
                else if (value == 2)
                {
                    x += rl.u8();
                    y += rl.u8();
                }
                else
                {
                    // An absolute run of `value` pixels, padded to 16 bits.
                    const std::uint32_t bytes = method == rle8 ? value : (value + 1u) / 2;
                    const auto run = rl.bytes(bytes);
                    if (!rl.ok())
                        break;
                    for (std::uint32_t i = 0; i < value; ++i)
                        put(method == rle8 ? run[i] : (i % 2 == 0 ? run[i / 2] >> 4 : run[i / 2] & 15u));
                    if (bytes % 2 != 0)
                        rl.skip(1);
                }
            }
            any_alpha = true;
        }
        else
        {
            const std::size_t stride = ((std::size_t{w} * bits + 31) / 32) * 4;
            if (pixels.size() < stride * (h - 1) + (std::size_t{w} * bits + 7) / 8)
                return failure(std::format("the BMP file is truncated: {} bytes of pixels where {} x {} needs {}",
                                           pixels.size(), w, h, stride * h));
            for (std::uint32_t stored = 0; stored < h; ++stored)
            {
                const std::uint8_t *row = pixels.data() + stride * stored;
                std::uint8_t *out = result.row(row_of(stored)).data();
                for (std::uint32_t x = 0; x < w; ++x, out += 4)
                {
                    if (bits <= 8)
                    {
                        const std::size_t bit = std::size_t{x} * bits;
                        const std::uint32_t index = (row[bit / 8] >> (8 - bits - bit % 8)) & ((1u << bits) - 1);
                        out[0] = palette[index][0];
                        out[1] = palette[index][1];
                        out[2] = palette[index][2];
                        out[3] = 255;
                        continue;
                    }
                    std::uint32_t value = 0;
                    const std::uint8_t *p = row + std::size_t{x} * (bits / 8);
                    for (std::uint32_t k = 0; k < bits / 8; ++k)
                        value |= std::uint32_t{p[k]} << (8 * k);
                    out[0] = channels[0].extract(value);
                    out[1] = channels[1].extract(value);
                    out[2] = channels[2].extract(value);
                    out[3] = channels[3].mask != 0 ? channels[3].extract(value) : std::uint8_t{255};
                    any_alpha = any_alpha || (channels[3].mask != 0 && out[3] != 0);
                }
            }
            // An alpha channel that is zero everywhere is padding that some
            // writers label alpha, not a picture that is all transparent.
            if (channels[3].mask != 0 && !any_alpha)
                for (std::size_t i = 3; i < result.size_bytes(); i += 4)
                    result.data()[i] = 255;
        }

        if (!any_alpha)
            return convert(result, pixel_format::rgb8);
        return result;
    }

} // namespace volt::image::bmp
