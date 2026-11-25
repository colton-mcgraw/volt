#include <image/jpeg.hpp>

#include "bytes.hpp"
#include "huffman.hpp"
#include "jpeg_common.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace volt::image::jpeg
{

    namespace
    {
        namespace tables = detail::jpeg;
        using failure = std::unexpected<std::string>;

        // A quantization table for a quality, scaled from the example table
        // as the IJG library does it, so qualities mean what users expect.
        std::array<std::uint16_t, 64> scaled_table(const std::uint8_t (&base)[64], int quality)
        {
            quality = std::clamp(quality, 1, 100);
            const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
            std::array<std::uint16_t, 64> result{};
            for (std::size_t i = 0; i < 64; ++i)
                result[i] = static_cast<std::uint16_t>(std::clamp((base[i] * scale + 50) / 100, 1, 255));
            return result;
        }

        // A component's samples at its own resolution, level-shifted, with
        // the edges replicated out to whole MCUs.
        struct plane
        {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::vector<float> samples;

            float at(std::uint32_t x, std::uint32_t y) const { return samples[std::size_t{y} * width + x]; }
        };

        // Quantized coefficients of every block of a component, zigzag order.
        struct coded_component
        {
            std::uint32_t h = 1;
            std::uint32_t v = 1;
            std::uint8_t table = 0; // quantization and Huffman tables (0 luma, 1 chroma)
            std::uint32_t blocks_wide = 0;
            std::uint32_t blocks_high = 0;
            std::vector<std::int16_t> blocks;

            const std::int16_t *block(std::uint32_t bx, std::uint32_t by) const
            {
                return blocks.data() + (std::size_t{by} * blocks_wide + bx) * 64;
            }
        };

        void forward_dct(const plane &source, std::uint32_t bx, std::uint32_t by,
                         const std::array<std::uint16_t, 64> &quant, std::int16_t *out)
        {
            const auto &basis = tables::dct_basis();
            float rows[64];
            // rows[y][u] = sum over x of basis[x][u] f[y][x]
            for (std::size_t y = 0; y < 8; ++y)
                for (std::size_t u = 0; u < 8; ++u)
                {
                    float sum = 0.0f;
                    for (std::size_t x = 0; x < 8; ++x)
                        sum += basis[x][u] * source.at(bx * 8 + static_cast<std::uint32_t>(x),
                                                       by * 8 + static_cast<std::uint32_t>(y));
                    rows[y * 8 + u] = sum;
                }
            for (std::size_t k = 0; k < 64; ++k)
            {
                const std::size_t natural = tables::zigzag[k];
                const std::size_t v = natural / 8;
                const std::size_t u = natural % 8;
                float sum = 0.0f;
                for (std::size_t y = 0; y < 8; ++y)
                    sum += basis[y][v] * rows[y * 8 + u];
                out[k] = static_cast<std::int16_t>(std::lround(sum / static_cast<float>(quant[natural])));
            }
        }

        // ---- entropy coding -----------------------------------------------------

        // The number of bits in a value's magnitude: its JPEG "category".
        unsigned category(int value)
        {
            unsigned magnitude = static_cast<unsigned>(value < 0 ? -value : value);
            unsigned bits = 0;
            while (magnitude != 0)
            {
                ++bits;
                magnitude >>= 1;
            }
            return bits;
        }

        struct huffman_code
        {
            std::uint8_t counts[16] = {};
            std::vector<std::uint8_t> values;
            std::uint16_t codes[256] = {};
            std::uint8_t lengths[256] = {};
        };

        huffman_code from_spec(const tables::huffman_spec &spec)
        {
            huffman_code result;
            std::copy(std::begin(spec.counts), std::end(spec.counts), result.counts);
            result.values.assign(spec.values, spec.values + spec.value_count);
            std::uint32_t code = 0;
            std::size_t index = 0;
            for (std::uint8_t length = 1; length <= 16; ++length)
            {
                for (int i = 0; i < spec.counts[length - 1]; ++i, ++index, ++code)
                {
                    result.codes[result.values[index]] = static_cast<std::uint16_t>(code);
                    result.lengths[result.values[index]] = length;
                }
                code <<= 1;
            }
            return result;
        }

        // An optimal code for the symbol frequencies, at most 16 bits long and
        // never using the all-ones code word (K.2): a dummy symbol, rarer than
        // any real one, gets one of the longest codes, and being the highest
        // symbol, the last of them, which is all ones.
        huffman_code from_frequencies(const std::array<std::uint32_t, 257> &frequencies)
        {
            std::array<std::uint32_t, 257> counted{};
            for (std::size_t s = 0; s < 256; ++s)
                counted[s] = 2 * frequencies[s];
            counted[256] = 1;
            const std::vector<std::uint8_t> lengths = volt::image::detail::huffman_lengths(counted, 16);

            tables::huffman_spec spec{};
            int count = 0;
            for (std::uint8_t length = 1; length <= 16; ++length)
                for (std::size_t symbol = 0; symbol < 256; ++symbol)
                    if (lengths[symbol] == length)
                    {
                        ++spec.counts[length - 1];
                        spec.values[count++] = static_cast<std::uint8_t>(symbol);
                    }
            spec.value_count = count;
            return from_spec(spec);
        }

        class bit_writer
        {
        public:
            explicit bit_writer(std::vector<std::uint8_t> &out) : m_out(out) {}

            void put(std::uint32_t value, unsigned count)
            {
                if (count == 0)
                    return;
                m_bits = (m_bits << count) | (value & ((1u << count) - 1));
                m_count += count;
                while (m_count >= 8)
                {
                    const auto byte = static_cast<std::uint8_t>(m_bits >> (m_count - 8));
                    m_out.push_back(byte);
                    if (byte == 0xFF)
                        m_out.push_back(0); // stuffing
                    m_count -= 8;
                }
            }

            // Pads the last byte with ones, as the standard asks.
            void flush()
            {
                if (m_count > 0)
                    put(0x7F, 8 - m_count);
            }

        private:
            std::vector<std::uint8_t> &m_out;
            std::uint64_t m_bits = 0;
            unsigned m_count = 0;
        };

        // Walks the symbols of one block: calls emit(table_kind, symbol,
        // extra_bits, extra_count) with table_kind 0 for DC and 1 for AC.
        template <typename F>
        void walk_block(const std::int16_t *block, int &previous_dc, F &&emit)
        {
            const int diff = block[0] - previous_dc;
            previous_dc = block[0];
            const unsigned dc_bits = category(diff);
            emit(0, dc_bits, static_cast<std::uint32_t>(diff < 0 ? diff - 1 : diff), dc_bits);

            unsigned run = 0;
            for (std::size_t k = 1; k < 64; ++k)
            {
                const int value = block[k];
                if (value == 0)
                {
                    ++run;
                    continue;
                }
                while (run >= 16)
                {
                    emit(1, 0xF0u, 0u, 0u);
                    run -= 16;
                }
                const unsigned bits = category(value);
                emit(1, (run << 4) | bits, static_cast<std::uint32_t>(value < 0 ? value - 1 : value), bits);
                run = 0;
            }
            if (run > 0)
                emit(1, 0x00u, 0u, 0u);
        }

        void put_segment(std::vector<std::uint8_t> &out, std::uint8_t marker, const std::vector<std::uint8_t> &body)
        {
            out.push_back(0xFF);
            out.push_back(marker);
            detail::put_be16(out, static_cast<std::uint32_t>(body.size() + 2));
            out.insert(out.end(), body.begin(), body.end());
        }
    } // namespace

    std::expected<std::vector<std::uint8_t>, std::string> encode(const image &input, const encode_options &options)
    {
        if (input.empty())
            return failure("cannot encode an empty image as JPEG");

        // 8-bit sRGB gray or RGB, straight alpha then dropped.
        image source = input;
        if (source.premultiplied())
            unpremultiply(source);
        if (source.encoding() != srgb_encoding)
            source = reencode(source, srgb_encoding);
        const bool gray = is_gray(source.format());
        source = convert(source, gray ? pixel_format::gray8 : pixel_format::rgb8);

        const std::uint32_t width = source.width();
        const std::uint32_t height = source.height();
        // Luma's sampling factors; chroma's are 1.
        std::uint32_t h_max = 1;
        std::uint32_t v_max = 1;
        if (!gray)
        {
            h_max = options.subsampling == chroma_subsampling::half ||
                            options.subsampling == chroma_subsampling::horizontal
                        ? 2
                        : 1;
            v_max = options.subsampling == chroma_subsampling::half ||
                            options.subsampling == chroma_subsampling::vertical
                        ? 2
                        : 1;
        }
        const std::uint32_t mcus_wide = (width + 8 * h_max - 1) / (8 * h_max);
        const std::uint32_t mcus_high = (height + 8 * v_max - 1) / (8 * v_max);

        // Full resolution planes: Y, Cb, Cr (JFIF's full-range BT.601), level
        // shifted, padded to whole MCUs by replicating the edges.
        const std::uint32_t padded_width = mcus_wide * 8 * h_max;
        const std::uint32_t padded_height = mcus_high * 8 * v_max;
        const std::size_t channels = gray ? 1 : 3;
        std::vector<plane> full(channels);
        for (plane &p : full)
        {
            p.width = padded_width;
            p.height = padded_height;
            p.samples.resize(std::size_t{padded_width} * padded_height);
        }
        for (std::uint32_t y = 0; y < padded_height; ++y)
            for (std::uint32_t x = 0; x < padded_width; ++x)
            {
                const std::uint8_t *pixel = source.at(std::min(x, width - 1), std::min(y, height - 1));
                const std::size_t i = std::size_t{y} * padded_width + x;
                if (gray)
                {
                    full[0].samples[i] = pixel[0] - 128.0f;
                    continue;
                }
                const float r = pixel[0];
                const float g = pixel[1];
                const float b = pixel[2];
                full[0].samples[i] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
                full[1].samples[i] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                full[2].samples[i] = 0.5f * r - 0.418688f * g - 0.081312f * b;
            }

        // Chroma reduced by averaging each h_max x v_max box, which puts its
        // samples centered between the luma samples they cover, where JFIF
        // says they are.
        if (h_max * v_max > 1)
            for (std::size_t c = 1; c < 3; ++c)
            {
                plane reduced;
                reduced.width = padded_width / h_max;
                reduced.height = padded_height / v_max;
                reduced.samples.resize(std::size_t{reduced.width} * reduced.height);
                const float weight = 1.0f / static_cast<float>(h_max * v_max);
                for (std::uint32_t y = 0; y < reduced.height; ++y)
                    for (std::uint32_t x = 0; x < reduced.width; ++x)
                    {
                        float sum = 0.0f;
                        for (std::uint32_t dy = 0; dy < v_max; ++dy)
                            for (std::uint32_t dx = 0; dx < h_max; ++dx)
                                sum += full[c].at(x * h_max + dx, y * v_max + dy);
                        reduced.samples[std::size_t{y} * reduced.width + x] = sum * weight;
                    }
                full[c] = std::move(reduced);
            }

        const std::array<std::uint16_t, 64> quant[2] = {scaled_table(tables::luminance_quantization, options.quality),
                                                         scaled_table(tables::chrominance_quantization, options.quality)};

        std::vector<coded_component> coded(channels);
        for (std::size_t c = 0; c < channels; ++c)
        {
            coded_component &cc = coded[c];
            cc.h = c == 0 ? h_max : 1;
            cc.v = c == 0 ? v_max : 1;
            cc.table = c == 0 ? 0 : 1;
            cc.blocks_wide = full[c].width / 8;
            cc.blocks_high = full[c].height / 8;
            cc.blocks.resize(std::size_t{cc.blocks_wide} * cc.blocks_high * 64);
            for (std::uint32_t by = 0; by < cc.blocks_high; ++by)
                for (std::uint32_t bx = 0; bx < cc.blocks_wide; ++bx)
                    forward_dct(full[c], bx, by, quant[cc.table],
                                cc.blocks.data() + (std::size_t{by} * cc.blocks_wide + bx) * 64);
        }

        // Every block in MCU order.
        const auto for_each_block = [&](auto &&visit) {
            for (std::uint32_t my = 0; my < mcus_high; ++my)
                for (std::uint32_t mx = 0; mx < mcus_wide; ++mx)
                    for (std::size_t c = 0; c < channels; ++c)
                    {
                        const coded_component &cc = coded[c];
                        for (std::uint32_t by = 0; by < cc.v; ++by)
                            for (std::uint32_t bx = 0; bx < cc.h; ++bx)
                                visit(c, cc.block(mx * cc.h + bx, my * cc.v + by));
                    }
        };

        // The Huffman codes: [table][dc, ac].
        huffman_code codes[2][2];
        if (options.optimize_huffman)
        {
            std::array<std::uint32_t, 257> frequencies[2][2] = {};
            std::vector<int> previous(channels, 0);
            for_each_block([&](std::size_t c, const std::int16_t *block) {
                const std::uint8_t table = coded[c].table;
                walk_block(block, previous[c], [&](int kind, unsigned symbol, std::uint32_t, unsigned) {
                    ++frequencies[table][kind][symbol];
                });
            });
            for (int t = 0; t < 2; ++t)
                for (int kind = 0; kind < 2; ++kind)
                    codes[t][kind] = from_frequencies(frequencies[t][kind]);
        }
        else
        {
            codes[0][0] = from_spec(tables::dc_luminance);
            codes[0][1] = from_spec(tables::ac_luminance);
            codes[1][0] = from_spec(tables::dc_chrominance);
            codes[1][1] = from_spec(tables::ac_chrominance);
        }

        // ---- the file ----------------------------------------------------------

        std::vector<std::uint8_t> out{0xFF, 0xD8};
        put_segment(out, 0xE0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});

        const std::size_t table_count = gray ? 1 : 2;
        std::vector<std::uint8_t> dqt;
        for (std::size_t t = 0; t < table_count; ++t)
        {
            dqt.push_back(static_cast<std::uint8_t>(t));
            for (std::size_t k = 0; k < 64; ++k)
                dqt.push_back(static_cast<std::uint8_t>(quant[t][tables::zigzag[k]]));
        }
        put_segment(out, 0xDB, dqt);

        std::vector<std::uint8_t> sof{8};
        detail::put_be16(sof, height);
        detail::put_be16(sof, width);
        sof.push_back(static_cast<std::uint8_t>(channels));
        for (std::size_t c = 0; c < channels; ++c)
        {
            sof.push_back(static_cast<std::uint8_t>(c + 1));
            sof.push_back(static_cast<std::uint8_t>(coded[c].h << 4 | coded[c].v));
            sof.push_back(coded[c].table);
        }
        put_segment(out, 0xC0, sof);

        std::vector<std::uint8_t> dht;
        for (std::size_t t = 0; t < table_count; ++t)
            for (int kind = 0; kind < 2; ++kind)
            {
                const huffman_code &code = codes[t][kind];
                dht.push_back(static_cast<std::uint8_t>(kind << 4 | static_cast<int>(t)));
                dht.insert(dht.end(), std::begin(code.counts), std::end(code.counts));
                dht.insert(dht.end(), code.values.begin(), code.values.end());
            }
        put_segment(out, 0xC4, dht);

        std::vector<std::uint8_t> sos{static_cast<std::uint8_t>(channels)};
        for (std::size_t c = 0; c < channels; ++c)
        {
            sos.push_back(static_cast<std::uint8_t>(c + 1));
            sos.push_back(static_cast<std::uint8_t>(coded[c].table << 4 | coded[c].table));
        }
        sos.push_back(0);
        sos.push_back(63);
        sos.push_back(0);
        put_segment(out, 0xDA, sos);

        bit_writer bits(out);
        std::vector<int> previous(channels, 0);
        for_each_block([&](std::size_t c, const std::int16_t *block) {
            const std::uint8_t table = coded[c].table;
            walk_block(block, previous[c],
                       [&](int kind, unsigned symbol, std::uint32_t extra, unsigned extra_count) {
                           const huffman_code &code = codes[table][kind];
                           bits.put(code.codes[symbol], code.lengths[symbol]);
                           bits.put(extra, extra_count);
                       });
        });
        bits.flush();

        out.push_back(0xFF);
        out.push_back(0xD9);
        return out;
    }

} // namespace volt::image::jpeg
