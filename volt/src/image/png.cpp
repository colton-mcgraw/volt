#include <image/png.hpp>

#include <image/compress.hpp>

#include "bytes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>

namespace volt::image::png
{

    namespace
    {
        constexpr std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

        using failure = std::unexpected<std::string>;

        enum color_type : std::uint8_t
        {
            gray = 0,
            rgb = 2,
            palette = 3,
            gray_alpha = 4,
            rgba = 6,
        };

        std::uint32_t samples_per_pixel(std::uint8_t type)
        {
            switch (type)
            {
            case gray:
            case palette:
                return 1;
            case gray_alpha:
                return 2;
            case rgb:
                return 3;
            default:
                return 4;
            }
        }

        struct header
        {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint8_t depth = 0;
            std::uint8_t type = 0;
            bool interlaced = false;

            std::uint32_t bits_per_pixel() const { return samples_per_pixel(type) * depth; }

            // Bytes per complete pixel, at least 1: how far back the filters
            // look.
            std::uint32_t filter_distance() const { return std::max(1u, bits_per_pixel() / 8); }

            std::size_t row_bytes(std::uint32_t pixels) const
            {
                return (std::size_t{pixels} * bits_per_pixel() + 7) / 8;
            }
        };

        std::string chunk_name(std::uint32_t type)
        {
            std::string name;
            for (int shift = 24; shift >= 0; shift -= 8)
            {
                const auto c = static_cast<char>((type >> shift) & 0xFFu);
                name += (c >= 32 && c < 127) ? c : '?';
            }
            return name;
        }

        constexpr std::uint32_t tag(const char (&name)[5])
        {
            return std::uint32_t(static_cast<unsigned char>(name[0])) << 24 |
                   std::uint32_t(static_cast<unsigned char>(name[1])) << 16 |
                   std::uint32_t(static_cast<unsigned char>(name[2])) << 8 |
                   std::uint32_t(static_cast<unsigned char>(name[3]));
        }

        // ---- Adam7 ---------------------------------------------------------------

        struct pass
        {
            std::uint32_t x0, y0, dx, dy;
        };

        constexpr pass adam7[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                   {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};

        std::uint32_t pass_size(std::uint32_t size, std::uint32_t start, std::uint32_t step)
        {
            return size > start ? (size - start + step - 1) / step : 0;
        }

        // ---- filters ---------------------------------------------------------------

        std::uint8_t paeth(std::uint8_t a, std::uint8_t b, std::uint8_t c)
        {
            const int p = a + b - c;
            const int pa = std::abs(p - a);
            const int pb = std::abs(p - b);
            const int pc = std::abs(p - c);
            if (pa <= pb && pa <= pc)
                return a;
            return pb <= pc ? b : c;
        }

        // Undoes a row's filter in place. `previous` is the unfiltered row
        // above, all zeros for the first row of a pass.
        bool unfilter(std::uint8_t type, std::uint8_t *row, const std::uint8_t *previous, std::size_t size,
                      std::size_t distance)
        {
            switch (type)
            {
            case 0:
                return true;
            case 1:
                for (std::size_t i = distance; i < size; ++i)
                    row[i] = static_cast<std::uint8_t>(row[i] + row[i - distance]);
                return true;
            case 2:
                for (std::size_t i = 0; i < size; ++i)
                    row[i] = static_cast<std::uint8_t>(row[i] + previous[i]);
                return true;
            case 3:
                for (std::size_t i = 0; i < size; ++i)
                {
                    const unsigned left = i >= distance ? row[i - distance] : 0u;
                    row[i] = static_cast<std::uint8_t>(row[i] + ((left + previous[i]) >> 1));
                }
                return true;
            case 4:
                for (std::size_t i = 0; i < size; ++i)
                {
                    const std::uint8_t left = i >= distance ? row[i - distance] : std::uint8_t{0};
                    const std::uint8_t corner = i >= distance ? previous[i - distance] : std::uint8_t{0};
                    row[i] = static_cast<std::uint8_t>(row[i] + paeth(left, previous[i], corner));
                }
                return true;
            default:
                return false;
            }
        }

        // ---- decoding ----------------------------------------------------------------

        struct decoder
        {
            header head;
            std::array<std::array<std::uint8_t, 4>, 256> colors{}; // palette, with alpha from tRNS
            std::uint32_t palette_size = 0;
            bool has_key = false;
            std::uint16_t key[3] = {}; // tRNS color key at the file's depth
            bool has_palette_alpha = false;
            pixel_format format = pixel_format::rgba8;

            void choose_format()
            {
                const bool sixteen = head.depth == 16;
                switch (head.type)
                {
                case gray:
                    format = make_format(has_key ? 2 : 1, sixteen);
                    break;
                case gray_alpha:
                    format = make_format(2, sixteen);
                    break;
                case rgb:
                    format = make_format(has_key ? 4 : 3, sixteen);
                    break;
                case palette:
                    format = has_palette_alpha ? pixel_format::rgba8 : pixel_format::rgb8;
                    break;
                default:
                    format = make_format(4, sixteen);
                    break;
                }
            }

            // One unfiltered row of `count` pixels, written to `out` as
            // `format` pixels `step` pixels apart.
            void expand(const std::uint8_t *row, std::uint32_t count, std::uint8_t *out, std::size_t step) const
            {
                const std::size_t size = pixel_size(format);
                const std::size_t advance = size * step;
                const std::uint32_t depth = head.depth;

                if (head.type == palette || (head.type == gray && depth < 8))
                {
                    const std::uint32_t mask = (1u << depth) - 1;
                    const std::uint32_t scale = depth == 8 ? 1u : 255u / mask;
                    for (std::uint32_t x = 0; x < count; ++x, out += advance)
                    {
                        const std::size_t bit = std::size_t{x} * depth;
                        const std::uint32_t value = depth == 8 ? row[x]
                                                               : (row[bit / 8] >> (8 - depth - bit % 8)) & mask;
                        if (head.type == palette)
                        {
                            const auto &color = colors[value];
                            std::memcpy(out, color.data(), size);
                            continue;
                        }
                        out[0] = static_cast<std::uint8_t>(value * scale);
                        if (has_key)
                            out[1] = value == key[0] ? 0 : 255;
                    }
                    return;
                }

                const std::uint32_t samples = samples_per_pixel(head.type);
                if (depth == 8)
                {
                    for (std::uint32_t x = 0; x < count; ++x, out += advance, row += samples)
                    {
                        std::memcpy(out, row, samples);
                        if (has_key)
                        {
                            const bool match = head.type == gray ? row[0] == key[0]
                                                                 : row[0] == key[0] && row[1] == key[1] &&
                                                                       row[2] == key[2];
                            out[samples] = match ? 0 : 255;
                        }
                    }
                    return;
                }

                // 16 bits: big-endian in the file, native in the image.
                for (std::uint32_t x = 0; x < count; ++x, out += advance, row += 2 * samples)
                {
                    std::uint16_t values[4] = {};
                    for (std::uint32_t s = 0; s < samples; ++s)
                    {
                        values[s] = static_cast<std::uint16_t>(row[2 * s] << 8 | row[2 * s + 1]);
                        store16(out + 2 * s, values[s]);
                    }
                    if (has_key)
                    {
                        const bool match = head.type == gray ? values[0] == key[0]
                                                             : values[0] == key[0] && values[1] == key[1] &&
                                                                   values[2] == key[2];
                        store16(out + 2 * samples, match ? std::uint16_t{0} : std::uint16_t{65535});
                    }
                }
            }
        };

        std::expected<header, std::string> read_header(std::span<const std::uint8_t> data)
        {
            if (data.size() != 13)
                return failure(std::format("the IHDR chunk is {} bytes long instead of 13", data.size()));
            detail::byte_reader in(data);
            header head;
            head.width = in.be32();
            head.height = in.be32();
            head.depth = in.u8();
            head.type = in.u8();
            const std::uint8_t compression = in.u8();
            const std::uint8_t filter = in.u8();
            const std::uint8_t interlace = in.u8();

            if (head.width == 0 || head.height == 0 || head.width > 0x7FFFFFFFu || head.height > 0x7FFFFFFFu)
                return failure(std::format("the PNG declares an invalid size of {} x {}", head.width, head.height));
            if (auto size = check_size(head.width, head.height); !size)
                return failure(std::move(size.error()));

            bool valid = false;
            switch (head.type)
            {
            case gray:
                valid = head.depth == 1 || head.depth == 2 || head.depth == 4 || head.depth == 8 || head.depth == 16;
                break;
            case palette:
                valid = head.depth == 1 || head.depth == 2 || head.depth == 4 || head.depth == 8;
                break;
            case rgb:
            case gray_alpha:
            case rgba:
                valid = head.depth == 8 || head.depth == 16;
                break;
            default:
                return failure(std::format("the PNG has the unknown color type {}", head.type));
            }
            if (!valid)
                return failure(std::format("the PNG has color type {} with the invalid bit depth {}", head.type, head.depth));
            if (compression != 0)
                return failure(std::format("the PNG uses the unknown compression method {}", compression));
            if (filter != 0)
                return failure(std::format("the PNG uses the unknown filter method {}", filter));
            if (interlace > 1)
                return failure(std::format("the PNG uses the unknown interlace method {}", interlace));
            head.interlaced = interlace == 1;
            return head;
        }

        color_encoding encoding_from(bool srgb_chunk, std::uint32_t gamma_chunk)
        {
            if (srgb_chunk || gamma_chunk == 0)
                return srgb_encoding;
            // gAMA holds the encoding exponent times 100000: 45455 is 1/2.2,
            // which files written for sRGB displays use.
            const double exponent = gamma_chunk / 100000.0;
            if (std::abs(exponent - 1.0 / 2.2) < 0.01)
                return srgb_encoding;
            if (std::abs(exponent - 1.0) < 0.01)
                return linear_encoding;
            return {transfer::gamma, static_cast<float>(1.0 / exponent)};
        }
    } // namespace

    bool is_png(std::span<const std::uint8_t> data) noexcept
    {
        return data.size() >= 8 && std::equal(std::begin(signature), std::end(signature), data.begin());
    }

    std::expected<image, std::string> decode(std::span<const std::uint8_t> data)
    {
        if (!is_png(data))
            return failure("the data is not a PNG file (the signature is missing)");

        decoder state;
        bool have_header = false;
        bool srgb_chunk = false;
        std::uint32_t gamma_chunk = 0;
        std::vector<std::uint8_t> compressed;
        bool ended = false;

        detail::byte_reader in(data);
        in.skip(8);
        while (!in.at_end())
        {
            const std::uint32_t length = in.be32();
            const std::size_t type_at = in.position();
            const std::uint32_t type = in.be32();
            if (!in.ok())
                return failure("the PNG file is truncated in a chunk header");
            if (length > 0x7FFFFFFFu)
                return failure(std::format("the {} chunk declares an invalid length", chunk_name(type)));
            const std::span<const std::uint8_t> body = in.bytes(length);
            const std::uint32_t stored_crc = in.be32();
            if (!in.ok())
                return failure(std::format("the PNG file is truncated in its {} chunk", chunk_name(type)));
            if (compress::crc32(data.subspan(type_at, 4 + std::size_t{length})) != stored_crc)
                return failure(std::format("the {} chunk is corrupt (its CRC does not match)", chunk_name(type)));

            if (!have_header && type != tag("IHDR"))
                return failure("the PNG file does not start with an IHDR chunk");

            if (type == tag("IHDR"))
            {
                if (have_header)
                    return failure("the PNG file has more than one IHDR chunk");
                auto head = read_header(body);
                if (!head)
                    return std::unexpected(std::move(head.error()));
                state.head = *head;
                have_header = true;
            }
            else if (type == tag("PLTE"))
            {
                if (length % 3 != 0 || length == 0 || length > 768)
                    return failure(std::format("the PLTE chunk has an invalid length of {}", length));
                state.palette_size = length / 3;
                for (std::uint32_t i = 0; i < state.palette_size; ++i)
                    state.colors[i] = {body[3 * i], body[3 * i + 1], body[3 * i + 2], 255};
            }
            else if (type == tag("tRNS"))
            {
                const header &head = state.head;
                if (head.type == palette)
                {
                    if (length > state.palette_size)
                        return failure("the tRNS chunk has more entries than the palette");
                    for (std::uint32_t i = 0; i < length; ++i)
                        state.colors[i][3] = body[i];
                    state.has_palette_alpha = length > 0;
                }
                else if (head.type == gray || head.type == rgb)
                {
                    const std::uint32_t samples = head.type == gray ? 1 : 3;
                    if (length != 2 * samples)
                        return failure(std::format("the tRNS chunk has an invalid length of {}", length));
                    for (std::uint32_t s = 0; s < samples; ++s)
                        state.key[s] = static_cast<std::uint16_t>(body[2 * s] << 8 | body[2 * s + 1]);
                    state.has_key = true;
                }
                // Images with alpha must not have tRNS; ignored if they do.
            }
            else if (type == tag("gAMA"))
            {
                if (length == 4)
                    gamma_chunk = detail::byte_reader(body).be32();
            }
            else if (type == tag("sRGB"))
            {
                srgb_chunk = true;
            }
            else if (type == tag("IDAT"))
            {
                compressed.insert(compressed.end(), body.begin(), body.end());
            }
            else if (type == tag("IEND"))
            {
                ended = true;
                break;
            }
            else if ((type >> 24 & 0x20u) == 0)
            {
                // An uppercase first letter: the chunk is critical, and
                // what it means cannot be ignored.
                return failure(std::format("the PNG file has an unknown critical chunk {}", chunk_name(type)));
            }
        }
        (void)ended; // A file that stops right after its data is still read.

        if (!have_header)
            return failure("the PNG file has no IHDR chunk");
        const header &head = state.head;
        if (head.type == palette && state.palette_size == 0)
            return failure("the PNG is a palette image without a PLTE chunk");
        if (compressed.empty())
            return failure("the PNG file has no image data (IDAT chunk)");

        // Unknown palette entries (a corrupt index) come out opaque black.
        if (head.type == palette)
            for (std::uint32_t i = state.palette_size; i < 256; ++i)
                state.colors[i] = {0, 0, 0, 255};

        // What the filtered rows add up to, per pass.
        std::size_t expected = 0;
        if (!head.interlaced)
            expected = std::size_t{head.height} * (head.row_bytes(head.width) + 1);
        else
            for (const pass &p : adam7)
            {
                const std::uint32_t w = pass_size(head.width, p.x0, p.dx);
                const std::uint32_t h = pass_size(head.height, p.y0, p.dy);
                if (w != 0 && h != 0)
                    expected += std::size_t{h} * (head.row_bytes(w) + 1);
            }

        // DEFLATE expands at most 1032 : 1, so too little data is known to
        // be too little before anything is allocated for it.
        if (compressed.size() * 1032 + 1024 < expected)
            return failure("the PNG's image data is far too short for its size (the file is truncated)");

        compress::inflate_options limits;
        limits.max_output = expected;
        limits.size_hint = expected;
        auto raw = compress::zlib_decompress(compressed, limits);
        if (!raw)
            return failure("the PNG's image data is corrupt: " + raw.error());
        if (raw->size() < expected)
            return failure(std::format("the PNG's image data is truncated ({} of {} bytes)", raw->size(), expected));

        state.choose_format();
        auto created = image::create(head.width, head.height, state.format, encoding_from(srgb_chunk, gamma_chunk));
        if (!created)
            return std::unexpected(std::move(created.error()));
        image result = std::move(*created);

        // Unfilter in place, pass by pass, and expand into the image.
        std::uint8_t *at = raw->data();
        const std::size_t distance = head.filter_distance();
        for (int index = 0; index < (head.interlaced ? 7 : 1); ++index)
        {
            const pass p = head.interlaced ? adam7[index] : pass{0, 0, 1, 1};
            const std::uint32_t w = pass_size(head.width, p.x0, p.dx);
            const std::uint32_t h = pass_size(head.height, p.y0, p.dy);
            if (w == 0 || h == 0)
                continue;

            const std::size_t row_size = head.row_bytes(w);
            const std::vector<std::uint8_t> zeros(row_size, 0);
            const std::uint8_t *previous = zeros.data();
            for (std::uint32_t y = 0; y < h; ++y)
            {
                const std::uint8_t filter = *at++;
                if (!unfilter(filter, at, previous, row_size, distance))
                    return failure(std::format("the PNG's row {} uses the unknown filter type {}", y, filter));
                state.expand(at, w, result.at(p.x0, p.y0 + y * p.dy), p.dx);
                previous = at;
                at += row_size;
            }
        }
        return result;
    }

    // ---- encoding --------------------------------------------------------------

    namespace
    {
        void put_chunk(std::vector<std::uint8_t> &out, const char (&name)[5], std::span<const std::uint8_t> body)
        {
            detail::put_be32(out, static_cast<std::uint32_t>(body.size()));
            const std::size_t start = out.size();
            detail::put_be32(out, tag(name));
            out.insert(out.end(), body.begin(), body.end());
            detail::put_be32(out, compress::crc32(std::span<const std::uint8_t>(out).subspan(start)));
        }

        // Filters `row` (with `previous` above it) with filter `type` into
        // `out`.
        void apply_filter(std::uint8_t type, const std::uint8_t *row, const std::uint8_t *previous, std::uint8_t *out,
                          std::size_t size, std::size_t distance)
        {
            for (std::size_t i = 0; i < size; ++i)
            {
                const std::uint8_t left = i >= distance ? row[i - distance] : std::uint8_t{0};
                const std::uint8_t up = previous[i];
                const std::uint8_t corner = i >= distance ? previous[i - distance] : std::uint8_t{0};
                std::uint8_t predicted = 0;
                switch (type)
                {
                case 1:
                    predicted = left;
                    break;
                case 2:
                    predicted = up;
                    break;
                case 3:
                    predicted = static_cast<std::uint8_t>((left + up) >> 1);
                    break;
                case 4:
                    predicted = paeth(left, up, corner);
                    break;
                default:
                    break;
                }
                out[i] = static_cast<std::uint8_t>(row[i] - predicted);
            }
        }

        std::uint64_t filter_cost(const std::uint8_t *row, std::size_t size)
        {
            std::uint64_t cost = 0;
            for (std::size_t i = 0; i < size; ++i)
                cost += static_cast<std::uint64_t>(std::abs(static_cast<int>(static_cast<std::int8_t>(row[i]))));
            return cost;
        }
    } // namespace

    std::expected<std::vector<std::uint8_t>, std::string> encode(const image &input, const encode_options &options)
    {
        if (input.empty())
            return failure("cannot encode an empty image as PNG");

        image straight;
        const image *source = &input;
        if (input.premultiplied() && has_alpha(input.format()))
        {
            straight = input;
            unpremultiply(straight);
            source = &straight;
        }

        const pixel_format format = source->format();
        const bool sixteen = channel_size(format) == 2;
        std::uint8_t type = rgba;
        switch (channel_count(format))
        {
        case 1:
            type = gray;
            break;
        case 2:
            type = gray_alpha;
            break;
        case 3:
            type = rgb;
            break;
        default:
            break;
        }

        std::vector<std::uint8_t> out(std::begin(signature), std::end(signature));

        std::vector<std::uint8_t> ihdr;
        detail::put_be32(ihdr, source->width());
        detail::put_be32(ihdr, source->height());
        ihdr.push_back(sixteen ? 16 : 8);
        ihdr.push_back(type);
        ihdr.push_back(0);
        ihdr.push_back(0);
        ihdr.push_back(0);
        put_chunk(out, "IHDR", ihdr);

        const color_encoding &encoding = source->encoding();
        if (encoding.curve == transfer::srgb)
        {
            // Perceptual intent, with the gAMA chunk the specification
            // recommends next to sRGB for older decoders.
            const std::uint8_t intent[] = {0};
            put_chunk(out, "sRGB", intent);
            std::vector<std::uint8_t> gamma;
            detail::put_be32(gamma, 45455);
            put_chunk(out, "gAMA", gamma);
        }
        else
        {
            const double exponent = encoding.curve == transfer::linear ? 1.0
                                                                       : 1.0 / std::max(0.01, static_cast<double>(encoding.gamma));
            std::vector<std::uint8_t> gamma;
            detail::put_be32(gamma, static_cast<std::uint32_t>(std::lround(exponent * 100000.0)));
            put_chunk(out, "gAMA", gamma);
        }

        // Rows in file order: big-endian samples, a filter byte in front.
        const std::size_t row_size = source->stride();
        const std::size_t distance = pixel_size(format);
        std::vector<std::uint8_t> filtered;
        filtered.reserve(source->height() * (row_size + 1));
        std::vector<std::uint8_t> current(row_size);
        std::vector<std::uint8_t> previous(row_size, 0);
        std::vector<std::uint8_t> candidate(row_size);
        std::vector<std::uint8_t> best(row_size);
        const bool adaptive = options.filter == filter_strategy::adaptive && options.compression > 0;

        for (std::uint32_t y = 0; y < source->height(); ++y)
        {
            const auto row = source->row(y);
            if (sixteen)
                for (std::size_t i = 0; i < row_size; i += 2)
                {
                    const std::uint16_t value = load16(row.data() + i);
                    current[i] = static_cast<std::uint8_t>(value >> 8);
                    current[i + 1] = static_cast<std::uint8_t>(value);
                }
            else
                std::memcpy(current.data(), row.data(), row_size);

            std::uint8_t chosen = 0;
            std::memcpy(best.data(), current.data(), row_size);
            if (adaptive)
            {
                std::uint64_t lowest = filter_cost(current.data(), row_size);
                for (std::uint8_t filter = 1; filter <= 4; ++filter)
                {
                    apply_filter(filter, current.data(), previous.data(), candidate.data(), row_size, distance);
                    const std::uint64_t cost = filter_cost(candidate.data(), row_size);
                    if (cost < lowest)
                    {
                        lowest = cost;
                        chosen = filter;
                        std::swap(best, candidate);
                    }
                }
            }
            filtered.push_back(chosen);
            filtered.insert(filtered.end(), best.begin(), best.end());
            std::swap(previous, current);
        }

        const std::vector<std::uint8_t> packed = compress::zlib_compress(filtered, options.compression);
        constexpr std::size_t chunk_limit = std::size_t{1} << 20;
        for (std::size_t at = 0; at < packed.size(); at += chunk_limit)
            put_chunk(out, "IDAT",
                      std::span<const std::uint8_t>(packed).subspan(at, std::min(chunk_limit, packed.size() - at)));
        put_chunk(out, "IEND", {});
        return out;
    }

} // namespace volt::image::png
