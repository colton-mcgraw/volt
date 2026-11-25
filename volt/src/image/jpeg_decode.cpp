#include <image/jpeg.hpp>

#include "bytes.hpp"
#include "jpeg_common.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <memory>
#include <string_view>

namespace volt::image::jpeg
{

    namespace
    {
        namespace tables = detail::jpeg;
        using failure = std::unexpected<std::string>;

        // Progressive files use about ten.
        constexpr int max_scans = 256;

        // ---- Huffman tables -----------------------------------------------------
        //
        // Codes of up to fast_bits resolve with one lookup; longer ones go
        // through the maxcode/valptr procedure of the standard's F.2.2.3.

        constexpr unsigned fast_bits = 9;

        struct huffman_table
        {
            bool defined = false;
            std::uint16_t fast[1u << fast_bits] = {}; // length << 8 | value; 0 if longer
            std::int32_t min_code[17] = {};
            std::int32_t max_code[18] = {};
            std::int32_t first_value[17] = {};
            std::uint8_t values[256] = {};

            bool build(const std::uint8_t (&counts)[16], std::span<const std::uint8_t> symbols)
            {
                std::fill(std::begin(fast), std::end(fast), std::uint16_t{0});
                std::copy(symbols.begin(), symbols.end(), values);

                std::int32_t code = 0;
                std::int32_t index = 0;
                for (int length = 1; length <= 16; ++length)
                {
                    const int count = counts[length - 1];
                    first_value[length] = index;
                    min_code[length] = code;
                    max_code[length] = count == 0 ? -1 : code + count - 1;
                    for (int i = 0; i < count; ++i, ++code, ++index)
                    {
                        if (code >= (1 << length))
                            return false; // more codes than the length has room for
                        if (static_cast<unsigned>(length) <= fast_bits)
                        {
                            const unsigned shift = fast_bits - static_cast<unsigned>(length);
                            const auto first = static_cast<unsigned>(code) << shift;
                            const auto entry = static_cast<std::uint16_t>(
                                static_cast<unsigned>(length) << 8 | values[static_cast<std::size_t>(index)]);
                            for (unsigned k = 0; k < (1u << shift); ++k)
                                fast[first + k] = entry;
                        }
                    }
                    code <<= 1;
                }
                max_code[17] = 0x7FFFFFFF;
                defined = true;
                return true;
            }
        };

        // ---- entropy-coded data ---------------------------------------------------
        //
        // Bits most significant first, with the 0xFF 0x00 stuffing removed.
        // At a marker, or at the end of the data, the reader stops and
        // supplies zero bits; overrun() tells whether any of those were used,
        // which means the scan's data ran out early.
        class entropy_reader
        {
        public:
            entropy_reader(std::span<const std::uint8_t> data, std::size_t position)
                : m_data(data), m_position(position)
            {
            }

            void fill() noexcept
            {
                while (m_count <= 56)
                {
                    std::uint64_t byte = 0;
                    if (!m_stopped && m_position < m_data.size())
                    {
                        byte = m_data[m_position];
                        if (byte != 0xFF)
                            ++m_position;
                        else if (m_position + 1 < m_data.size() && m_data[m_position + 1] == 0)
                            m_position += 2;
                        else
                        {
                            m_stopped = true; // a marker: leave it for the parser
                            byte = 0;
                        }
                    }
                    else
                        m_stopped = true;
                    if (m_stopped)
                        m_padding += 8;
                    m_bits |= byte << (56 - m_count);
                    m_count += 8;
                }
            }

            std::uint32_t peek16() noexcept
            {
                if (m_count < 16)
                    fill();
                return static_cast<std::uint32_t>(m_bits >> 48);
            }

            void consume(unsigned count) noexcept
            {
                m_bits <<= count;
                m_count -= count;
            }

            // `count` up to 16.
            std::uint32_t bits(unsigned count) noexcept
            {
                if (count == 0)
                    return 0;
                if (m_count < count)
                    fill();
                const auto value = static_cast<std::uint32_t>(m_bits >> (64 - count));
                consume(count);
                return value;
            }

            std::uint32_t bit() noexcept { return bits(1); }

            // The next `size` bits as a signed value (the standard's
            // EXTEND(RECEIVE(size))).
            std::int32_t receive(unsigned size) noexcept
            {
                if (size == 0)
                    return 0;
                const auto value = static_cast<std::int32_t>(bits(size));
                return value < (1 << (size - 1)) ? value - (1 << size) + 1 : value;
            }

            // The next symbol, or -1 for bits that are no code of the table.
            int decode(const huffman_table &table) noexcept
            {
                const std::uint32_t look = peek16();
                const std::uint16_t entry = table.fast[look >> (16 - fast_bits)];
                if (entry != 0)
                {
                    consume(entry >> 8);
                    return entry & 0xFF;
                }
                for (unsigned length = fast_bits + 1; length <= 16; ++length)
                {
                    const auto code = static_cast<std::int32_t>(look >> (16 - length));
                    if (code <= table.max_code[length])
                    {
                        consume(length);
                        const std::int32_t index = table.first_value[length] + code - table.min_code[length];
                        return table.values[static_cast<std::size_t>(index) & 0xFFu];
                    }
                }
                return -1;
            }

            bool overrun() const noexcept { return m_padding > m_count; }

            // Expects a restart marker where the data is: skips it and starts
            // afresh. false if there is none.
            bool restart() noexcept
            {
                m_bits = 0;
                m_count = 0;
                m_padding = 0;
                m_stopped = false;

                // Damage can leave the reader short of the marker; look ahead.
                while (m_position + 1 < m_data.size())
                {
                    if (m_data[m_position] == 0xFF)
                    {
                        const std::uint8_t next = m_data[m_position + 1];
                        if (next >= 0xD0 && next <= 0xD7)
                        {
                            m_position += 2;
                            return true;
                        }
                        if (next != 0 && next != 0xFF)
                            return false; // some other marker: the scan is over
                    }
                    ++m_position;
                }
                return false;
            }

            std::size_t position() const noexcept { return m_position; }

        private:
            std::span<const std::uint8_t> m_data;
            std::size_t m_position;
            std::uint64_t m_bits = 0;
            unsigned m_count = 0;
            unsigned m_padding = 0;
            bool m_stopped = false;
        };

        // ---- the frame ------------------------------------------------------------

        struct component
        {
            std::uint8_t id = 0;
            std::uint32_t h = 1;
            std::uint32_t v = 1;
            std::uint8_t quant_table = 0;

            std::uint32_t width = 0;  // samples that hold picture
            std::uint32_t height = 0;
            std::uint32_t blocks_wide = 0; // blocks with picture in them
            std::uint32_t blocks_high = 0;
            std::uint32_t stride = 0;      // blocks allocated per row (whole MCUs)
            std::uint32_t rows = 0;
            std::vector<std::int16_t> coefficients; // 64 per block, natural order

            std::array<std::uint16_t, 64> quant{};
            bool has_quant = false;

            // Per scan.
            std::int32_t dc_prediction = 0;
            std::uint8_t dc_table = 0;
            std::uint8_t ac_table = 0;

            std::int16_t *block(std::uint32_t bx, std::uint32_t by)
            {
                return coefficients.data() + (std::size_t{by} * stride + bx) * 64;
            }
        };

        std::int16_t clamp16(std::int32_t value)
        {
            return static_cast<std::int16_t>(std::clamp(value, -32768, 32767));
        }

        struct decoder
        {
            std::span<const std::uint8_t> data;

            huffman_table dc_tables[4];
            huffman_table ac_tables[4];
            std::array<std::uint16_t, 64> quant_tables[4]{};
            bool quant_defined[4] = {};
            std::uint32_t restart_interval = 0;

            bool have_frame = false;
            bool progressive = false;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::vector<component> components;
            std::uint32_t h_max = 1;
            std::uint32_t v_max = 1;
            std::uint32_t mcus_wide = 0;
            std::uint32_t mcus_high = 0;

            int scans = 0;
            bool damaged = false; // some scan ended before its data did

            int adobe_transform = -1; // -1: no Adobe marker
            bool jfif = false;
            int orientation = 1;

            // The scan being decoded.
            std::vector<component *> scan;
            std::uint32_t spectral_start = 0;
            std::uint32_t spectral_end = 63;
            std::uint32_t approximation_high = 0;
            std::uint32_t approximation_low = 0;
            std::uint32_t eob_run = 0;

            // ---- segments ---------------------------------------------------------

            std::expected<void, std::string> read_frame(std::span<const std::uint8_t> body, std::uint8_t marker)
            {
                if (have_frame)
                    return failure("the JPEG has more than one frame header");
                detail::byte_reader in(body);
                const std::uint8_t precision = in.u8();
                height = in.be16();
                width = in.be16();
                const std::uint8_t count = in.u8();
                if (!in.ok())
                    return failure("the JPEG's frame header is truncated");
                if (precision != 8)
                    return failure(std::format("the JPEG has {}-bit samples; only 8-bit JPEG is supported", precision));
                if (height == 0)
                    return failure("the JPEG defines its height in a DNL marker, which is not supported");
                if (width == 0)
                    return failure("the JPEG has a width of zero");
                if (auto size = check_size(width, height); !size)
                    return failure(std::move(size.error()));
                if (count != 1 && count != 3 && count != 4)
                    return failure(std::format("the JPEG has {} color components; 1, 3 or 4 are supported", count));

                progressive = marker == 0xC2;
                components.resize(count);
                for (component &c : components)
                {
                    c.id = in.u8();
                    const std::uint8_t sampling = in.u8();
                    c.h = sampling >> 4;
                    c.v = sampling & 15u;
                    c.quant_table = in.u8();
                    if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4)
                        return failure(std::format("the JPEG has invalid sampling factors {}x{}", c.h, c.v));
                    if (c.quant_table > 3)
                        return failure("the JPEG's frame header names an invalid quantization table");
                    h_max = std::max(h_max, c.h);
                    v_max = std::max(v_max, c.v);
                }
                if (!in.ok())
                    return failure("the JPEG's frame header is truncated");

                mcus_wide = (width + 8 * h_max - 1) / (8 * h_max);
                mcus_high = (height + 8 * v_max - 1) / (8 * v_max);
                std::uint64_t blocks = 0;
                for (component &c : components)
                {
                    if (h_max % c.h != 0 || v_max % c.v != 0)
                        return failure(std::format("the JPEG's sampling factors ({}x{} against {}x{}) are not integral "
                                                   "ratios, which is not supported",
                                                   c.h, c.v, h_max, v_max));
                    c.width = (width * c.h + h_max - 1) / h_max;
                    c.height = (height * c.v + v_max - 1) / v_max;
                    c.blocks_wide = (c.width + 7) / 8;
                    c.blocks_high = (c.height + 7) / 8;
                    c.stride = mcus_wide * c.h;
                    c.rows = mcus_high * c.v;
                    blocks += std::uint64_t{c.blocks_wide} * c.blocks_high;
                }

                // Every block takes at least one bit (its DC code) in some
                // scan, so a file needs that many bits. Checked before the
                // coefficients are allocated, which a few bytes of header
                // could otherwise make gigabytes of.
                if (blocks > std::uint64_t{data.size()} * 8)
                    return failure(std::format("the JPEG declares {} x {} pixels, far more than its {} bytes can hold",
                                               width, height, data.size()));
                for (component &c : components)
                    c.coefficients.assign(std::size_t{c.stride} * c.rows * 64, 0);
                have_frame = true;
                return {};
            }

            std::expected<void, std::string> read_huffman_tables(std::span<const std::uint8_t> body)
            {
                detail::byte_reader in(body);
                while (!in.at_end())
                {
                    const std::uint8_t which = in.u8();
                    std::uint8_t counts[16];
                    std::size_t total = 0;
                    for (std::uint8_t &count : counts)
                    {
                        count = in.u8();
                        total += count;
                    }
                    const auto symbols = in.bytes(total);
                    if (!in.ok())
                        return failure("a JPEG Huffman table is truncated");
                    if ((which >> 4) > 1 || (which & 15u) > 3)
                        return failure("a JPEG Huffman table has an invalid class or number");
                    if (total > 256)
                        return failure("a JPEG Huffman table has more than 256 codes");
                    huffman_table &table = (which >> 4) == 0 ? dc_tables[which & 3u] : ac_tables[which & 3u];
                    if (!table.build(counts, symbols))
                        return failure("a JPEG Huffman table has more codes than its lengths allow");
                }
                return {};
            }

            std::expected<void, std::string> read_quantization_tables(std::span<const std::uint8_t> body)
            {
                detail::byte_reader in(body);
                while (!in.at_end())
                {
                    const std::uint8_t which = in.u8();
                    const bool sixteen = (which >> 4) != 0;
                    const std::uint8_t index = which & 15u;
                    if ((which >> 4) > 1 || index > 3)
                        return failure("a JPEG quantization table has an invalid precision or number");
                    for (std::size_t k = 0; k < 64; ++k)
                        quant_tables[index][tables::zigzag[k]] = sixteen ? in.be16() : in.u8();
                    if (!in.ok())
                        return failure("a JPEG quantization table is truncated");
                    quant_defined[index] = true;
                }
                return {};
            }

            void read_app(std::uint8_t marker, std::span<const std::uint8_t> body)
            {
                const auto starts = [&](std::string_view prefix) {
                    return body.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), body.begin());
                };
                if (marker == 0xE0 && starts(std::string_view("JFIF\0", 5)))
                    jfif = true;
                else if (marker == 0xEE && starts("Adobe") && body.size() >= 12)
                    adobe_transform = body[11];
                else if (marker == 0xE1 && starts(std::string_view("Exif\0\0", 6)))
                    orientation = read_orientation(body.subspan(6));
            }

            // Tag 0x0112 of the TIFF structure inside an Exif segment, 1 if
            // absent or unreadable.
            static int read_orientation(std::span<const std::uint8_t> tiff)
            {
                if (tiff.size() < 8)
                    return 1;
                const bool little = tiff[0] == 'I' && tiff[1] == 'I';
                if (!little && !(tiff[0] == 'M' && tiff[1] == 'M'))
                    return 1;
                detail::byte_reader in(tiff);
                const auto u16 = [&] { return little ? in.le16() : in.be16(); };
                const auto u32 = [&] { return little ? in.le32() : in.be32(); };
                in.skip(2);
                if (u16() != 42)
                    return 1;
                in.seek(u32());
                const std::uint32_t entries = u16();
                for (std::uint32_t i = 0; i < entries && in.ok(); ++i)
                {
                    const std::uint16_t tag = u16();
                    const std::uint16_t type = u16();
                    const std::uint32_t count = u32();
                    const std::uint16_t value = u16();
                    in.skip(2);
                    if (tag == 0x0112 && type == 3 && count == 1 && in.ok())
                        return value >= 1 && value <= 8 ? value : 1;
                }
                return 1;
            }

            std::expected<void, std::string> read_scan_header(std::span<const std::uint8_t> body)
            {
                if (!have_frame)
                    return failure("the JPEG has a scan before its frame header");
                detail::byte_reader in(body);
                const std::uint8_t count = in.u8();
                if (count < 1 || count > 4)
                    return failure("a JPEG scan has an invalid number of components");
                scan.clear();
                for (std::uint8_t i = 0; i < count; ++i)
                {
                    const std::uint8_t id = in.u8();
                    const std::uint8_t which = in.u8();
                    auto found = std::find_if(components.begin(), components.end(),
                                              [&](const component &c) { return c.id == id; });
                    if (found == components.end())
                        return failure(std::format("a JPEG scan names component {}, which the frame does not have", id));
                    if (std::find(scan.begin(), scan.end(), &*found) != scan.end())
                        return failure("a JPEG scan names a component twice");
                    found->dc_table = which >> 4;
                    found->ac_table = which & 15u;
                    if (found->dc_table > 3 || found->ac_table > 3)
                        return failure("a JPEG scan names an invalid Huffman table");
                    scan.push_back(&*found);
                }
                spectral_start = in.u8();
                spectral_end = in.u8();
                const std::uint8_t approximation = in.u8();
                approximation_high = approximation >> 4;
                approximation_low = approximation & 15u;
                if (!in.ok())
                    return failure("a JPEG scan header is truncated");

                if (progressive)
                {
                    if (spectral_start > 63 || spectral_end > 63 || spectral_start > spectral_end ||
                        (spectral_start == 0 && spectral_end != 0) || approximation_low > 13 ||
                        approximation_high > 13)
                        return failure("a progressive JPEG scan has invalid spectral or approximation parameters");
                    if (spectral_start > 0 && count != 1)
                        return failure("a progressive JPEG scan of AC coefficients has more than one component");
                }
                else
                {
                    spectral_start = 0;
                    spectral_end = 63;
                    approximation_high = 0;
                    approximation_low = 0;
                }

                for (component *c : scan)
                {
                    if (!c->has_quant)
                    {
                        if (!quant_defined[c->quant_table])
                            return failure(std::format("the JPEG uses quantization table {} without defining it",
                                                       c->quant_table));
                        c->quant = quant_tables[c->quant_table];
                        c->has_quant = true;
                    }
                    const bool needs_dc = spectral_start == 0 && approximation_high == 0;
                    const bool needs_ac = spectral_end > 0;
                    if (needs_dc && !dc_tables[c->dc_table].defined)
                        return failure("a JPEG scan uses a DC Huffman table that is not defined");
                    if (needs_ac && !ac_tables[c->ac_table].defined)
                        return failure("a JPEG scan uses an AC Huffman table that is not defined");
                }
                return {};
            }

            // ---- blocks -------------------------------------------------------------
            //
            // Each returns false for bits that make no sense, which ends the
            // scan.

            bool decode_block(entropy_reader &in, component &c, std::int16_t *block)
            {
                const int size = in.decode(dc_tables[c.dc_table]);
                if (size < 0 || size > 16)
                    return false;
                c.dc_prediction = std::clamp(c.dc_prediction + in.receive(static_cast<unsigned>(size)), -65536, 65536);
                block[0] = clamp16(c.dc_prediction);

                const huffman_table &ac = ac_tables[c.ac_table];
                for (unsigned k = 1; k < 64;)
                {
                    const int symbol = in.decode(ac);
                    if (symbol < 0)
                        return false;
                    const unsigned run = static_cast<unsigned>(symbol) >> 4;
                    const unsigned bits = static_cast<unsigned>(symbol) & 15u;
                    if (bits == 0)
                    {
                        if (run != 15)
                            break; // end of block
                        k += 16;
                        continue;
                    }
                    k += run;
                    block[tables::zigzag[k]] = clamp16(in.receive(bits));
                    ++k;
                }
                return true;
            }

            bool decode_dc_first(entropy_reader &in, component &c, std::int16_t *block)
            {
                const int size = in.decode(dc_tables[c.dc_table]);
                if (size < 0 || size > 16)
                    return false;
                c.dc_prediction = std::clamp(c.dc_prediction + in.receive(static_cast<unsigned>(size)), -65536, 65536);
                block[0] = clamp16(c.dc_prediction * (1 << approximation_low));
                return true;
            }

            bool decode_dc_refine(entropy_reader &in, std::int16_t *block)
            {
                if (in.bit())
                    block[0] = static_cast<std::int16_t>(block[0] | (1 << approximation_low));
                return true;
            }

            bool decode_ac_first(entropy_reader &in, component &c, std::int16_t *block)
            {
                if (eob_run > 0)
                {
                    --eob_run;
                    return true;
                }
                const huffman_table &ac = ac_tables[c.ac_table];
                for (unsigned k = spectral_start; k <= spectral_end;)
                {
                    const int symbol = in.decode(ac);
                    if (symbol < 0)
                        return false;
                    const unsigned run = static_cast<unsigned>(symbol) >> 4;
                    const unsigned bits = static_cast<unsigned>(symbol) & 15u;
                    if (bits == 0)
                    {
                        if (run < 15)
                        {
                            eob_run = (1u << run) - 1;
                            if (run != 0)
                                eob_run += in.bits(run);
                            break;
                        }
                        k += 16;
                        continue;
                    }
                    k += run;
                    block[tables::zigzag[k]] = clamp16(in.receive(bits) * (1 << approximation_low));
                    ++k;
                }
                return true;
            }

            // The successive approximation of AC coefficients (G.1.2.3): new
            // coefficients of magnitude 1, and a correction bit for each one
            // that is already nonzero, interleaved.
            bool decode_ac_refine(entropy_reader &in, component &c, std::int16_t *block)
            {
                const int one = 1 << approximation_low;
                const auto refine = [&](std::int16_t &value) {
                    if (in.bit() && (value & one) == 0)
                        value = static_cast<std::int16_t>(value >= 0 ? value + one : value - one);
                };

                unsigned k = spectral_start;
                if (eob_run == 0)
                {
                    const huffman_table &ac = ac_tables[c.ac_table];
                    while (k <= spectral_end)
                    {
                        const int symbol = in.decode(ac);
                        if (symbol < 0)
                            return false;
                        unsigned run = static_cast<unsigned>(symbol) >> 4;
                        const unsigned bits = static_cast<unsigned>(symbol) & 15u;
                        int value = 0;
                        if (bits == 0)
                        {
                            if (run < 15)
                            {
                                eob_run = 1u << run;
                                if (run != 0)
                                    eob_run += in.bits(run);
                                break; // the rest of the block is refined below
                            }
                            // run 15: sixteen zeros, nothing new
                        }
                        else
                        {
                            if (bits != 1)
                                return false;
                            value = in.bit() ? one : -one;
                        }

                        while (k <= spectral_end)
                        {
                            std::int16_t &coefficient = block[tables::zigzag[k++]];
                            if (coefficient != 0)
                                refine(coefficient);
                            else
                            {
                                if (run == 0)
                                {
                                    coefficient = static_cast<std::int16_t>(value);
                                    break;
                                }
                                --run;
                            }
                        }
                    }
                }

                if (eob_run > 0)
                {
                    for (; k <= spectral_end; ++k)
                    {
                        std::int16_t &coefficient = block[tables::zigzag[k]];
                        if (coefficient != 0)
                            refine(coefficient);
                    }
                    --eob_run;
                }
                return true;
            }

            bool decode_unit(entropy_reader &in, component &c, std::int16_t *block)
            {
                if (!progressive)
                    return decode_block(in, c, block);
                if (spectral_start == 0)
                    return approximation_high == 0 ? decode_dc_first(in, c, block) : decode_dc_refine(in, block);
                return approximation_high == 0 ? decode_ac_first(in, c, block) : decode_ac_refine(in, c, block);
            }

            // Decodes the scan whose data starts at `position`; returns where
            // the data ended.
            std::size_t decode_scan(std::size_t position)
            {
                entropy_reader in(data, position);
                for (component *c : scan)
                    c->dc_prediction = 0;
                eob_run = 0;

                // One component alone is coded block by block over the part of
                // it with picture; several go in whole MCUs.
                const bool interleaved = scan.size() > 1;
                const std::uint32_t units_wide = interleaved ? mcus_wide : scan[0]->blocks_wide;
                const std::uint32_t units_high = interleaved ? mcus_high : scan[0]->blocks_high;
                const std::uint64_t units = std::uint64_t{units_wide} * units_high;

                for (std::uint64_t unit = 0; unit < units; ++unit)
                {
                    if (restart_interval != 0 && unit != 0 && unit % restart_interval == 0)
                    {
                        if (!in.restart())
                        {
                            damaged = true;
                            break;
                        }
                        for (component *c : scan)
                            c->dc_prediction = 0;
                        eob_run = 0;
                    }

                    const auto ux = static_cast<std::uint32_t>(unit % units_wide);
                    const auto uy = static_cast<std::uint32_t>(unit / units_wide);
                    bool ok = true;
                    if (!interleaved)
                        ok = decode_unit(in, *scan[0], scan[0]->block(ux, uy));
                    else
                        for (component *c : scan)
                            for (std::uint32_t by = 0; by < c->v && ok; ++by)
                                for (std::uint32_t bx = 0; bx < c->h && ok; ++bx)
                                    ok = decode_unit(in, *c, c->block(ux * c->h + bx, uy * c->v + by));
                    if (!ok || in.overrun())
                    {
                        damaged = true;
                        break;
                    }
                }
                ++scans;
                return in.position();
            }

            // ---- pixels -------------------------------------------------------------

            // The component's samples, block_size * blocks: the inverse DCT of
            // every block.
            static std::vector<std::uint8_t> reconstruct(const component &c)
            {
                const auto &basis = tables::dct_basis();
                const std::size_t plane_width = std::size_t{c.stride} * 8;
                std::vector<std::uint8_t> plane(plane_width * c.rows * 8);
                float rows[64];
                for (std::uint32_t by = 0; by < c.rows; ++by)
                    for (std::uint32_t bx = 0; bx < c.stride; ++bx)
                    {
                        const std::int16_t *block = c.coefficients.data() + (std::size_t{by} * c.stride + bx) * 64;

                        // Rows: rows[v][x] = sum over u of basis[x][u] F[v][u].
                        for (std::size_t v = 0; v < 8; ++v)
                        {
                            float f[8];
                            bool flat = true;
                            for (std::size_t u = 0; u < 8; ++u)
                            {
                                f[u] = static_cast<float>(block[v * 8 + u]) * static_cast<float>(c.quant[v * 8 + u]);
                                flat = flat && (u == 0 || block[v * 8 + u] == 0);
                            }
                            for (std::size_t x = 0; x < 8; ++x)
                            {
                                float sum = basis[x][0] * f[0];
                                if (!flat)
                                    for (std::size_t u = 1; u < 8; ++u)
                                        sum += basis[x][u] * f[u];
                                rows[v * 8 + x] = sum;
                            }
                        }

                        // Columns, then level shift and rounding.
                        std::uint8_t *out = plane.data() + std::size_t{by} * 8 * plane_width + std::size_t{bx} * 8;
                        for (std::size_t y = 0; y < 8; ++y, out += plane_width)
                            for (std::size_t x = 0; x < 8; ++x)
                            {
                                float sum = 128.0f + 0.5f;
                                for (std::size_t v = 0; v < 8; ++v)
                                    sum += basis[y][v] * rows[v * 8 + x];
                                out[x] = static_cast<std::uint8_t>(std::clamp(sum, 0.0f, 255.0f));
                            }
                    }
                return plane;
            }

            // A component's samples at full resolution: linear interpolation
            // between sample centers (chroma sits centered between the luma
            // samples it covers, as JFIF says), replicating the edges.
            std::vector<std::uint8_t> upsample(const component &c, const std::vector<std::uint8_t> &plane) const
            {
                const std::uint32_t sx = h_max / c.h;
                const std::uint32_t sy = v_max / c.v;
                const std::size_t plane_width = std::size_t{c.stride} * 8;
                std::vector<std::uint8_t> full(std::size_t{width} * height);

                if (sx == 1 && sy == 1)
                {
                    for (std::uint32_t y = 0; y < height; ++y)
                        std::memcpy(full.data() + std::size_t{y} * width, plane.data() + y * plane_width, width);
                    return full;
                }

                // Where each output sample falls between two source samples,
                // with the weight of the second in 1/256.
                struct tap
                {
                    std::uint32_t first, second, weight;
                };
                const auto taps = [](std::uint32_t count, std::uint32_t factor, std::uint32_t limit) {
                    std::vector<tap> result(count);
                    for (std::uint32_t i = 0; i < count; ++i)
                    {
                        // Source position (i + 0.5) / factor - 0.5, in 1/256.
                        const std::int64_t at = (std::int64_t{2 * i + 1} * 256 - std::int64_t{factor} * 256) /
                                                (2 * std::int64_t{factor});
                        const std::int64_t base = at >= 0 ? at / 256 : -1;
                        const auto weight = static_cast<std::uint32_t>(at - base * 256);
                        const auto first = static_cast<std::uint32_t>(std::clamp<std::int64_t>(base, 0, limit - 1));
                        const auto second = static_cast<std::uint32_t>(std::clamp<std::int64_t>(base + 1, 0, limit - 1));
                        result[i] = {first, second, weight};
                    }
                    return result;
                };
                const std::vector<tap> across = taps(width, sx, c.width);
                const std::vector<tap> down = taps(height, sy, c.height);

                // Each source row widened once, kept for the two output rows
                // that need it.
                std::vector<std::uint32_t> wide_rows[2] = {std::vector<std::uint32_t>(width),
                                                           std::vector<std::uint32_t>(width)};
                std::uint32_t cached[2] = {~0u, ~0u};
                const auto widened = [&](std::uint32_t source_row) -> const std::uint32_t * {
                    for (int k = 0; k < 2; ++k)
                        if (cached[k] == source_row)
                            return wide_rows[k].data();
                    const int slot = cached[0] == ~0u || (cached[1] != ~0u && cached[0] < cached[1]) ? 0 : 1;
                    cached[slot] = source_row;
                    const std::uint8_t *in = plane.data() + std::size_t{source_row} * plane_width;
                    std::uint32_t *out = wide_rows[slot].data();
                    for (std::uint32_t x = 0; x < width; ++x)
                    {
                        const tap &t = across[x];
                        out[x] = in[t.first] * (256 - t.weight) + in[t.second] * t.weight;
                    }
                    return out;
                };

                for (std::uint32_t y = 0; y < height; ++y)
                {
                    const tap &t = down[y];
                    const std::uint32_t *top = widened(t.first);
                    const std::uint32_t *bottom = widened(t.second);
                    std::uint8_t *out = full.data() + std::size_t{y} * width;
                    for (std::uint32_t x = 0; x < width; ++x)
                        out[x] = static_cast<std::uint8_t>((top[x] * (256 - t.weight) + bottom[x] * t.weight + 32768) >> 16);
                }
                return full;
            }

            std::expected<image, std::string> output(const decode_options &options)
            {
                std::vector<std::vector<std::uint8_t>> planes;
                for (const component &c : components)
                    planes.push_back(upsample(c, reconstruct(c)));

                const bool gray = components.size() == 1;
                auto created = image::create(width, height, gray ? pixel_format::gray8 : pixel_format::rgb8);
                if (!created)
                    return std::unexpected(std::move(created.error()));
                image result = std::move(*created);

                if (gray)
                    std::memcpy(result.data(), planes[0].data(), planes[0].size());
                else
                {
                    // What the three or four components are.
                    bool ycc = true;
                    if (components.size() == 3)
                    {
                        if (adobe_transform >= 0)
                            ycc = adobe_transform != 0;
                        else if (!jfif && components[0].id == 'R' && components[1].id == 'G' && components[2].id == 'B')
                            ycc = false;
                    }
                    else
                        ycc = adobe_transform == 2; // YCCK; otherwise CMYK

                    const std::size_t count = std::size_t{width} * height;
                    std::uint8_t *out = result.data();
                    for (std::size_t i = 0; i < count; ++i, out += 3)
                    {
                        int r = planes[0][i];
                        int g = planes[1][i];
                        int b = planes[2][i];
                        if (ycc)
                        {
                            // JFIF's full-range BT.601, in 16.16 fixed point.
                            const int y = r * 65536 + 32768;
                            const int cb = g - 128;
                            const int cr = b - 128;
                            r = (y + 91881 * cr) >> 16;
                            g = (y - 22554 * cb - 46802 * cr) >> 16;
                            b = (y + 116130 * cb) >> 16;
                        }
                        r = std::clamp(r, 0, 255);
                        g = std::clamp(g, 0, 255);
                        b = std::clamp(b, 0, 255);
                        if (components.size() == 4)
                        {
                            // Adobe writes CMYK inverted, so the stored values
                            // are 255 - C and 255 - K: multiplying them gives
                            // the color directly. YCCK converts to (inverted
                            // again) CMY first.
                            const int k = planes[3][i];
                            if (ycc)
                            {
                                r = 255 - r;
                                g = 255 - g;
                                b = 255 - b;
                            }
                            r = (r * k + 127) / 255;
                            g = (g * k + 127) / 255;
                            b = (b * k + 127) / 255;
                        }
                        out[0] = static_cast<std::uint8_t>(r);
                        out[1] = static_cast<std::uint8_t>(g);
                        out[2] = static_cast<std::uint8_t>(b);
                    }
                }

                if (options.apply_orientation && orientation != 1)
                    return apply_orientation(result, orientation);
                return result;
            }
        };
    } // namespace

    bool is_jpeg(std::span<const std::uint8_t> data) noexcept
    {
        return data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
    }

    std::expected<image, std::string> decode(std::span<const std::uint8_t> data, const decode_options &options)
    {
        if (data.size() < 2 || data[0] != 0xFF || data[1] != 0xD8)
            return failure("the data is not a JPEG file (it does not start with an SOI marker)");

        auto state = std::make_unique<decoder>();
        state->data = data;

        // A file cut short after the picture started still shows what it has.
        const auto truncated = [&](const char *where) -> std::expected<image, std::string> {
            if (state->scans > 0)
                return state->output(options);
            return failure(std::format("the JPEG file is truncated {}", where));
        };

        std::size_t at = 2;
        for (;;)
        {
            // The next marker, past any fill bytes (and, leniently, past
            // garbage between segments).
            while (at < data.size() && data[at] != 0xFF)
                ++at;
            while (at < data.size() && data[at] == 0xFF)
                ++at;
            if (at >= data.size())
                return truncated("before its end marker");
            const std::uint8_t marker = data[at++];

            if (marker == 0xD9)
                break; // EOI
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x00)
                continue; // stand-alone markers

            if (at + 2 > data.size())
                return truncated("in a segment header");
            const std::size_t length = std::size_t{data[at]} << 8 | data[at + 1];
            if (length < 2)
                return failure(std::format("a JPEG segment (marker 0x{:02X}) has an invalid length", marker));
            if (at + length > data.size())
                return truncated("in a segment");
            const std::span<const std::uint8_t> body = data.subspan(at + 2, length - 2);
            at += length;

            std::expected<void, std::string> result;
            switch (marker)
            {
            case 0xC0: // baseline
            case 0xC1: // extended sequential
            case 0xC2: // progressive
                result = state->read_frame(body, marker);
                break;
            case 0xC3:
                return failure("the JPEG is lossless, which is not supported");
            case 0xC5:
            case 0xC6:
            case 0xC7:
                return failure("the JPEG is hierarchical, which is not supported");
            case 0xC9:
            case 0xCA:
            case 0xCB:
            case 0xCD:
            case 0xCE:
            case 0xCF:
            case 0xCC:
                return failure("the JPEG uses arithmetic coding, which is not supported");
            case 0xC4:
                result = state->read_huffman_tables(body);
                break;
            case 0xDB:
                result = state->read_quantization_tables(body);
                break;
            case 0xDD:
                if (body.size() < 2)
                    return failure("the JPEG's restart interval segment is too short");
                state->restart_interval = std::uint32_t{body[0]} << 8 | body[1];
                break;
            case 0xDA:
                // Each scan goes over the whole picture again: a file of
                // nothing but scan headers would take forever.
                if (state->scans >= max_scans)
                    return truncated("(it has more scans than any real file)");
                result = state->read_scan_header(body);
                if (result)
                    at = state->decode_scan(at);
                break;
            default:
                if (marker >= 0xE0 && marker <= 0xEF)
                    state->read_app(marker, body);
                // COM, DNL and anything unknown: skipped.
                break;
            }
            if (!result)
                return std::unexpected(std::move(result.error()));
        }

        if (!state->have_frame)
            return failure("the JPEG file has no frame header");
        if (state->scans == 0)
            return failure("the JPEG file has no image data (no scan)");
        return state->output(options);
    }

} // namespace volt::image::jpeg
