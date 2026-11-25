#include <image/image.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace volt::image
{

    const char *name(pixel_format format) noexcept
    {
        switch (format)
        {
        case pixel_format::gray8:
            return "gray8";
        case pixel_format::gray_alpha8:
            return "gray_alpha8";
        case pixel_format::rgb8:
            return "rgb8";
        case pixel_format::rgba8:
            return "rgba8";
        case pixel_format::gray16:
            return "gray16";
        case pixel_format::gray_alpha16:
            return "gray_alpha16";
        case pixel_format::rgb16:
            return "rgb16";
        case pixel_format::rgba16:
            return "rgba16";
        }
        return "unknown";
    }

    // ---- color encoding -----------------------------------------------------

    float to_linear(const color_encoding &encoding, float encoded) noexcept
    {
        switch (encoding.curve)
        {
        case transfer::srgb:
            return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        case transfer::linear:
            return encoded;
        case transfer::gamma:
            return encoded <= 0.0f ? 0.0f : std::pow(encoded, encoding.gamma);
        }
        return encoded;
    }

    float from_linear(const color_encoding &encoding, float linear) noexcept
    {
        switch (encoding.curve)
        {
        case transfer::srgb:
            return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        case transfer::linear:
            return linear;
        case transfer::gamma:
            return linear <= 0.0f || encoding.gamma <= 0.0f ? 0.0f : std::pow(linear, 1.0f / encoding.gamma);
        }
        return linear;
    }

    std::expected<void, std::string> check_size(std::uint64_t width, std::uint64_t height)
    {
        if (width == 0 || height == 0)
            return std::unexpected(std::format("the image is empty ({} x {} pixels)", width, height));
        if (width > max_dimension || height > max_dimension)
            return std::unexpected(std::format("the image is {} x {} pixels, more than the {} allowed in either direction",
                                               width, height, max_dimension));
        if (width * height > max_pixels)
            return std::unexpected(std::format("the image is {} x {} pixels, more than the {} allowed in all", width,
                                               height, max_pixels));
        return {};
    }

    std::expected<image, std::string> image::create(std::uint32_t width, std::uint32_t height, pixel_format format,
                                                    color_encoding encoding)
    {
        if (auto checked = check_size(width, height); !checked)
            return std::unexpected(std::move(checked.error()));

        image result;
        result.m_width = width;
        result.m_height = height;
        result.m_format = format;
        result.m_encoding = encoding;
        result.m_pixels.assign(std::size_t{width} * height * pixel_size(format), 0);
        return result;
    }

    namespace
    {
        // An image of the same size and encoding in another format. The size
        // was valid for the source, so it is for this.
        image blank_like(const image &source, pixel_format format)
        {
            auto result = image::create(source.width(), source.height(), format, source.encoding());
            result->set_premultiplied(source.premultiplied());
            return std::move(*result);
        }

        // ---- 16-bit rows: layout conversion ---------------------------------

        void read_row16(const image &source, std::uint32_t y, std::uint16_t *rgba)
        {
            const std::uint8_t *in = source.row(y).data();
            const std::uint32_t channels = channel_count(source.format());
            const bool sixteen = channel_size(source.format()) == 2;
            for (std::uint32_t x = 0; x < source.width(); ++x)
            {
                std::uint16_t v[4] = {0, 0, 0, 65535};
                for (std::uint32_t c = 0; c < channels; ++c)
                {
                    if (sixteen)
                        v[c] = load16(in + 2 * c);
                    else
                        v[c] = static_cast<std::uint16_t>(in[c] * 257u);
                }
                in += pixel_size(source.format());

                std::uint16_t *out = rgba + 4 * std::size_t{x};
                if (channels <= 2)
                {
                    out[0] = out[1] = out[2] = v[0];
                    out[3] = channels == 2 ? v[1] : std::uint16_t{65535};
                }
                else
                {
                    out[0] = v[0];
                    out[1] = v[1];
                    out[2] = v[2];
                    out[3] = v[3];
                }
            }
        }

        void write_row16(image &target, std::uint32_t y, const std::uint16_t *rgba)
        {
            std::uint8_t *out = target.row(y).data();
            const std::uint32_t channels = channel_count(target.format());
            const bool sixteen = channel_size(target.format()) == 2;
            for (std::uint32_t x = 0; x < target.width(); ++x)
            {
                const std::uint16_t *in = rgba + 4 * std::size_t{x};
                std::uint16_t v[4];
                if (channels <= 2)
                {
                    // BT.709 luma, weights summing to 65536.
                    const std::uint32_t luma = (13933u * in[0] + 46871u * in[1] + 4732u * in[2] + 32768u) >> 16;
                    v[0] = static_cast<std::uint16_t>(std::min(luma, 65535u));
                    v[1] = in[3];
                }
                else
                {
                    v[0] = in[0];
                    v[1] = in[1];
                    v[2] = in[2];
                    v[3] = in[3];
                }
                for (std::uint32_t c = 0; c < channels; ++c)
                {
                    if (sixteen)
                        store16(out + 2 * c, v[c]);
                    else
                        out[c] = static_cast<std::uint8_t>((v[c] * 255u + 32767u) / 65535u);
                }
                out += pixel_size(target.format());
            }
        }

        // ---- float rows: linear light ---------------------------------------
        //
        // Rows as linear RGBA floats. Color comes out exactly as stored as far
        // as alpha goes: premultiplied images give premultiplied values, which
        // works because premultiplication happens in linear light.

        class linear_codec
        {
        public:
            linear_codec(const color_encoding &encoding, bool sixteen) : m_encoding(encoding), m_sixteen(sixteen)
            {
                if (encoding.curve == transfer::linear)
                    return;
                const std::uint32_t levels = sixteen ? 65536u : 256u;
                const float top = static_cast<float>(levels - 1);
                m_decode.resize(levels);
                for (std::uint32_t i = 0; i < levels; ++i)
                    m_decode[i] = to_linear(encoding, static_cast<float>(i) / top);

                // For 8 bits, encoding is a search among the midpoints between
                // levels, which rounds exactly as round(from_linear(v) * 255)
                // would, without a pow per value.
                if (!sixteen)
                {
                    m_thresholds.resize(255);
                    for (std::uint32_t i = 0; i < 255; ++i)
                        m_thresholds[i] = to_linear(encoding, (static_cast<float>(i) + 0.5f) / 255.0f);
                }
            }

            float decode(std::uint32_t value) const noexcept
            {
                if (m_decode.empty())
                    return static_cast<float>(value) / (m_sixteen ? 65535.0f : 255.0f);
                return m_decode[value];
            }

            std::uint16_t encode(float linear) const noexcept
            {
                const float top = m_sixteen ? 65535.0f : 255.0f;
                if (!(linear > 0.0f))
                    return 0;
                if (linear >= 1.0f)
                    return static_cast<std::uint16_t>(top);
                if (m_thresholds.empty())
                {
                    const float encoded = from_linear(m_encoding, linear);
                    return static_cast<std::uint16_t>(std::clamp(encoded, 0.0f, 1.0f) * top + 0.5f);
                }
                const auto found = std::upper_bound(m_thresholds.begin(), m_thresholds.end(), linear);
                return static_cast<std::uint16_t>(found - m_thresholds.begin());
            }

        private:
            color_encoding m_encoding;
            bool m_sixteen;
            std::vector<float> m_decode;
            std::vector<float> m_thresholds;
        };

        void read_row_linear(const image &source, const linear_codec &codec, std::uint32_t y, float *rgba)
        {
            const std::uint8_t *in = source.row(y).data();
            const std::uint32_t channels = channel_count(source.format());
            const bool sixteen = channel_size(source.format()) == 2;
            const float top = sixteen ? 65535.0f : 255.0f;
            const auto value = [&](std::uint32_t c) -> std::uint32_t { return sixteen ? load16(in + 2 * c) : in[c]; };

            for (std::uint32_t x = 0; x < source.width(); ++x)
            {
                float *out = rgba + 4 * std::size_t{x};
                if (channels <= 2)
                {
                    out[0] = out[1] = out[2] = codec.decode(value(0));
                    out[3] = channels == 2 ? static_cast<float>(value(1)) / top : 1.0f;
                }
                else
                {
                    out[0] = codec.decode(value(0));
                    out[1] = codec.decode(value(1));
                    out[2] = codec.decode(value(2));
                    out[3] = channels == 4 ? static_cast<float>(value(3)) / top : 1.0f;
                }
                in += pixel_size(source.format());
            }
        }

        void write_row_linear(image &target, const linear_codec &codec, std::uint32_t y, const float *rgba)
        {
            std::uint8_t *out = target.row(y).data();
            const std::uint32_t channels = channel_count(target.format());
            const bool sixteen = channel_size(target.format()) == 2;
            const float top = sixteen ? 65535.0f : 255.0f;
            const auto put = [&](std::uint32_t c, std::uint16_t v) {
                if (sixteen)
                    store16(out + 2 * c, v);
                else
                    out[c] = static_cast<std::uint8_t>(v);
            };
            const auto alpha = [&](float a) { return static_cast<std::uint16_t>(std::clamp(a, 0.0f, 1.0f) * top + 0.5f); };

            for (std::uint32_t x = 0; x < target.width(); ++x)
            {
                const float *in = rgba + 4 * std::size_t{x};
                if (channels <= 2)
                {
                    put(0, codec.encode(0.2126f * in[0] + 0.7152f * in[1] + 0.0722f * in[2]));
                    if (channels == 2)
                        put(1, alpha(in[3]));
                }
                else
                {
                    put(0, codec.encode(in[0]));
                    put(1, codec.encode(in[1]));
                    put(2, codec.encode(in[2]));
                    if (channels == 4)
                        put(3, alpha(in[3]));
                }
                out += pixel_size(target.format());
            }
        }

        // Rewrites every row through linear light with `change` applied to
        // each RGBA pixel, from `source`'s encoding to `target`'s.
        template <typename F>
        void transform_linear(const image &source, image &target, F &&change)
        {
            const bool sixteen_in = channel_size(source.format()) == 2;
            const bool sixteen_out = channel_size(target.format()) == 2;
            const linear_codec in(source.encoding(), sixteen_in);
            const linear_codec out(target.encoding(), sixteen_out);
            std::vector<float> row(4 * std::size_t{source.width()});
            for (std::uint32_t y = 0; y < source.height(); ++y)
            {
                read_row_linear(source, in, y, row.data());
                for (std::size_t x = 0; x < source.width(); ++x)
                    change(row.data() + 4 * x);
                write_row_linear(target, out, y, row.data());
            }
        }
    } // namespace

    image convert(const image &source, pixel_format format)
    {
        if (source.empty())
            return {};
        if (source.format() == format)
            return source;

        image result = blank_like(source, format);
        std::vector<std::uint16_t> row(4 * std::size_t{source.width()});
        for (std::uint32_t y = 0; y < source.height(); ++y)
        {
            read_row16(source, y, row.data());
            write_row16(result, y, row.data());
        }
        return result;
    }

    image reencode(const image &source, const color_encoding &encoding)
    {
        if (source.empty())
            return {};
        if (source.encoding() == encoding)
            return source;

        image result = blank_like(source, source.format());
        result.set_encoding(encoding);
        transform_linear(source, result, [](float *) {});
        return result;
    }

    image to_rgba8_srgb(const image &source)
    {
        if (source.empty())
            return {};

        // Straight alpha first: unpremultiplying at 8 bits after a re-encode
        // would lose more than doing it at the source's precision.
        image result = source;
        if (result.premultiplied())
            unpremultiply(result);
        if (result.encoding() != srgb_encoding)
        {
            // Re-encoding at 16 bits keeps the dark end of a linear image.
            if (channel_size(result.format()) == 1)
                result = convert(result, with_depth(result.format(), true));
            result = reencode(result, srgb_encoding);
        }
        return convert(result, pixel_format::rgba8);
    }

    void premultiply(image &target)
    {
        if (target.empty() || !has_alpha(target.format()) || target.premultiplied())
            return;
        transform_linear(target, target, [](float *p) {
            p[0] *= p[3];
            p[1] *= p[3];
            p[2] *= p[3];
        });
        target.set_premultiplied(true);
    }

    void unpremultiply(image &target)
    {
        if (target.empty() || !has_alpha(target.format()) || !target.premultiplied())
            return;
        transform_linear(target, target, [](float *p) {
            if (p[3] <= 0.0f)
            {
                p[0] = p[1] = p[2] = 0.0f;
                return;
            }
            p[0] = std::min(p[0] / p[3], 1.0f);
            p[1] = std::min(p[1] / p[3], 1.0f);
            p[2] = std::min(p[2] / p[3], 1.0f);
        });
        target.set_premultiplied(false);
    }

    // ---- scaling ------------------------------------------------------------

    namespace
    {
        // Which source samples make up each destination sample, and with
        // what weight: the overlap of the destination's footprint with each.
        struct box_tap
        {
            std::uint32_t first = 0;
            std::uint32_t count = 0;
            std::size_t weights = 0; // offset into box_filter::weights
        };

        struct box_filter
        {
            std::vector<box_tap> taps;
            std::vector<float> weights;
        };

        box_filter make_box_filter(std::uint32_t source_size, std::uint32_t target_size)
        {
            box_filter filter;
            filter.taps.resize(target_size);
            const double scale = static_cast<double>(source_size) / static_cast<double>(target_size);
            for (std::uint32_t i = 0; i < target_size; ++i)
            {
                const double begin = i * scale;
                const double end = (i + 1) * scale;
                auto first = static_cast<std::uint32_t>(std::floor(begin));
                auto last = static_cast<std::uint32_t>(std::ceil(end));
                first = std::min(first, source_size - 1);
                last = std::clamp(last, first + 1, source_size);

                box_tap &tap = filter.taps[i];
                tap.first = first;
                tap.count = last - first;
                tap.weights = filter.weights.size();
                double total = 0.0;
                for (std::uint32_t s = first; s < last; ++s)
                {
                    const double overlap = std::min(end, s + 1.0) - std::max(begin, static_cast<double>(s));
                    filter.weights.push_back(static_cast<float>(std::max(overlap, 0.0)));
                    total += std::max(overlap, 0.0);
                }
                for (std::uint32_t k = 0; k < tap.count; ++k)
                    filter.weights[tap.weights + k] =
                        total > 0.0 ? static_cast<float>(static_cast<double>(filter.weights[tap.weights + k]) / total)
                                    : 1.0f / static_cast<float>(tap.count);
            }
            return filter;
        }
    } // namespace

    image resize(const image &source, std::uint32_t width, std::uint32_t height)
    {
        if (source.empty() || width == 0 || height == 0)
            return {};
        if (width == source.width() && height == source.height())
            return source;
        if (!check_size(width, height))
            return {};

        const bool straight = !source.premultiplied();
        const linear_codec in(source.encoding(), channel_size(source.format()) == 2);

        // Horizontal pass into premultiplied linear rows of the new width.
        const box_filter across = make_box_filter(source.width(), width);
        std::vector<float> wide(4 * std::size_t{source.width()});
        std::vector<float> narrow(4 * std::size_t{width} * source.height());
        for (std::uint32_t y = 0; y < source.height(); ++y)
        {
            read_row_linear(source, in, y, wide.data());
            if (straight)
            {
                for (std::size_t x = 0; x < source.width(); ++x)
                {
                    float *p = wide.data() + 4 * x;
                    p[0] *= p[3];
                    p[1] *= p[3];
                    p[2] *= p[3];
                }
            }
            float *out = narrow.data() + 4 * std::size_t{width} * y;
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const box_tap &tap = across.taps[x];
                float sum[4] = {0, 0, 0, 0};
                for (std::uint32_t k = 0; k < tap.count; ++k)
                {
                    const float weight = across.weights[tap.weights + k];
                    const float *p = wide.data() + 4 * std::size_t{tap.first + k};
                    for (int c = 0; c < 4; ++c)
                        sum[c] += weight * p[c];
                }
                for (int c = 0; c < 4; ++c)
                    out[4 * x + static_cast<std::uint32_t>(c)] = sum[c];
            }
        }

        // Vertical pass, one destination row at a time.
        auto sized = image::create(width, height, source.format(), source.encoding());
        image result = std::move(*sized);
        result.set_premultiplied(source.premultiplied());

        const linear_codec out(result.encoding(), channel_size(result.format()) == 2);
        const box_filter down = make_box_filter(source.height(), height);
        std::vector<float> row(4 * std::size_t{width});
        for (std::uint32_t y = 0; y < height; ++y)
        {
            std::fill(row.begin(), row.end(), 0.0f);
            const box_tap &tap = down.taps[y];
            for (std::uint32_t k = 0; k < tap.count; ++k)
            {
                const float weight = down.weights[tap.weights + k];
                const float *line = narrow.data() + 4 * std::size_t{width} * (tap.first + k);
                for (std::size_t i = 0; i < row.size(); ++i)
                    row[i] += weight * line[i];
            }
            if (straight)
            {
                for (std::size_t x = 0; x < width; ++x)
                {
                    float *p = row.data() + 4 * x;
                    if (p[3] > 0.0f)
                    {
                        p[0] /= p[3];
                        p[1] /= p[3];
                        p[2] /= p[3];
                    }
                }
            }
            write_row_linear(result, out, y, row.data());
        }
        return result;
    }

    image downsample(const image &source)
    {
        if (source.empty())
            return {};
        return resize(source, std::max(source.width() / 2, 1u), std::max(source.height() / 2, 1u));
    }

    std::vector<image> mip_chain(const image &source, std::uint32_t levels)
    {
        std::vector<image> chain;
        if (source.empty())
            return chain;
        chain.push_back(source);
        while ((levels == 0 || chain.size() < levels) && (chain.back().width() > 1 || chain.back().height() > 1))
            chain.push_back(downsample(chain.back()));
        return chain;
    }

    // ---- orientation ----------------------------------------------------------

    void flip_vertical(image &target)
    {
        if (target.empty())
            return;
        std::vector<std::uint8_t> swap(target.stride());
        for (std::uint32_t top = 0, bottom = target.height() - 1; top < bottom; ++top, --bottom)
        {
            std::memcpy(swap.data(), target.row(top).data(), swap.size());
            std::memcpy(target.row(top).data(), target.row(bottom).data(), swap.size());
            std::memcpy(target.row(bottom).data(), swap.data(), swap.size());
        }
    }

    image apply_orientation(const image &source, int orientation)
    {
        if (source.empty() || orientation <= 1 || orientation > 8)
            return source;

        const std::uint32_t w = source.width();
        const std::uint32_t h = source.height();
        const bool swapped = orientation >= 5;
        auto created = image::create(swapped ? h : w, swapped ? w : h, source.format(), source.encoding());
        image result = std::move(*created);
        result.set_premultiplied(source.premultiplied());

        const std::size_t size = pixel_size(source.format());
        for (std::uint32_t y = 0; y < result.height(); ++y)
        {
            for (std::uint32_t x = 0; x < result.width(); ++x)
            {
                // Where the pixel shown at (x, y) is stored.
                std::uint32_t sx = x;
                std::uint32_t sy = y;
                switch (orientation)
                {
                case 2:
                    sx = w - 1 - x;
                    break;
                case 3:
                    sx = w - 1 - x;
                    sy = h - 1 - y;
                    break;
                case 4:
                    sy = h - 1 - y;
                    break;
                case 5:
                    sx = y;
                    sy = x;
                    break;
                case 6:
                    sx = y;
                    sy = h - 1 - x;
                    break;
                case 7:
                    sx = w - 1 - y;
                    sy = h - 1 - x;
                    break;
                case 8:
                    sx = w - 1 - y;
                    sy = x;
                    break;
                default:
                    break;
                }
                std::memcpy(result.at(x, y), source.at(sx, sy), size);
            }
        }
        return result;
    }

} // namespace volt::image
