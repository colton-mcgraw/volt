#include "inflate.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <memory>

namespace volt::compress
{

    namespace
    {
        // ---- bits, least significant first ----------------------------------
        //
        // Reading past the end yields zero bits rather than failing on the
        // spot, which keeps the decoding loop free of checks per bit; the
        // loop asks exhausted() once per symbol instead.
        class bit_input
        {
        public:
            explicit bit_input(std::span<const std::uint8_t> data) : m_data(data) {}

            void refill() noexcept
            {
                while (m_count <= 56)
                {
                    const std::uint64_t byte = m_position < m_data.size() ? m_data[m_position] : 0u;
                    ++m_position;
                    m_bits |= byte << m_count;
                    m_count += 8;
                }
            }

            // At most 56 bits, after a refill().
            std::uint32_t peek(unsigned count) const noexcept
            {
                return static_cast<std::uint32_t>(m_bits & ((std::uint64_t{1} << count) - 1));
            }

            void consume(unsigned count) noexcept
            {
                m_bits >>= count;
                m_count -= count;
            }

            std::uint32_t bits(unsigned count) noexcept
            {
                if (count == 0)
                    return 0;
                if (m_count < count)
                    refill();
                const std::uint32_t value = peek(count);
                consume(count);
                return value;
            }

            void align_to_byte() noexcept { consume(m_count % 8); }

            // Bits taken so far, real or not.
            std::uint64_t consumed() const noexcept { return std::uint64_t{m_position} * 8 - m_count; }

            bool exhausted() const noexcept { return consumed() > std::uint64_t{m_data.size()} * 8; }

        private:
            std::span<const std::uint8_t> m_data;
            std::size_t m_position = 0;
            std::uint64_t m_bits = 0;
            unsigned m_count = 0;
        };

        // ---- Huffman decoding -------------------------------------------------
        //
        // Codes of up to fast_bits are resolved by one table lookup on the
        // next bits; longer ones (rare: they are the rare symbols) by walking
        // the canonical code a bit at a time.

        constexpr unsigned fast_bits = 10;
        constexpr unsigned max_bits = 15;

        class huffman
        {
        public:
            // false for an over-subscribed set of lengths, or an incomplete
            // one with more than a single code (as zlib decides), unless
            // `incomplete` allows it: the fixed distance code is one.
            bool build(const std::uint8_t *lengths, std::size_t count, bool incomplete = false) noexcept
            {
                std::fill(std::begin(m_count), std::end(m_count), std::uint16_t{0});
                std::fill(std::begin(m_fast), std::end(m_fast), std::uint16_t{0});
                for (std::size_t s = 0; s < count; ++s)
                    ++m_count[lengths[s]];
                m_count[0] = 0;

                int left = 1;
                unsigned longest = 0;
                for (unsigned length = 1; length <= max_bits; ++length)
                {
                    left <<= 1;
                    left -= m_count[length];
                    if (left < 0)
                        return false;
                    if (m_count[length] != 0)
                        longest = length;
                }
                if (left > 0 && longest > 1 && !incomplete)
                    return false;
                m_empty = longest == 0;

                std::uint16_t offsets[max_bits + 2] = {};
                for (unsigned length = 1; length <= max_bits; ++length)
                    offsets[length + 1] = static_cast<std::uint16_t>(offsets[length] + m_count[length]);
                for (std::size_t s = 0; s < count; ++s)
                    if (lengths[s] != 0)
                        m_symbol[offsets[lengths[s]]++] = static_cast<std::uint16_t>(s);

                // The fast table: canonical codes, reversed because they
                // arrive most significant bit first in an LSB-first stream.
                std::uint32_t code = 0;
                std::size_t index = 0;
                for (unsigned length = 1; length <= fast_bits; ++length)
                {
                    for (unsigned k = 0; k < m_count[length]; ++k, ++code, ++index)
                    {
                        std::uint32_t reversed = 0;
                        for (unsigned b = 0; b < length; ++b)
                            reversed |= ((code >> b) & 1u) << (length - 1 - b);
                        const auto entry = static_cast<std::uint16_t>((std::uint32_t{m_symbol[index]} << 4) | length);
                        for (std::uint32_t fill = reversed; fill < (1u << fast_bits); fill += 1u << length)
                            m_fast[fill] = entry;
                    }
                    code <<= 1;
                }
                return true;
            }

            // The next symbol, or -1 if the bits are no code of this set.
            // The input must have been refilled.
            int decode(bit_input &input) const noexcept
            {
                const std::uint16_t entry = m_fast[input.peek(fast_bits)];
                if (entry != 0)
                {
                    input.consume(entry & 15u);
                    return entry >> 4;
                }

                // Canonical decoding, as in zlib's puff.
                const std::uint32_t bits = input.peek(max_bits);
                int code = 0;
                int first = 0;
                int index = 0;
                for (unsigned length = 1; length <= max_bits; ++length)
                {
                    code |= static_cast<int>((bits >> (length - 1)) & 1u);
                    const int count = m_count[length];
                    if (code - first < count)
                    {
                        input.consume(length);
                        return m_symbol[static_cast<std::size_t>(index + code - first)];
                    }
                    index += count;
                    first += count;
                    first <<= 1;
                    code <<= 1;
                }
                return -1;
            }

            bool empty() const noexcept { return m_empty; }

        private:
            std::uint16_t m_fast[1u << fast_bits] = {};
            std::uint16_t m_count[max_bits + 1] = {};
            std::uint16_t m_symbol[288] = {};
            bool m_empty = true;
        };

        constexpr std::uint16_t length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr std::uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr std::uint16_t distance_base[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                     33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr std::uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

        // The output so far, grown geometrically, and its limit.
        class output_buffer
        {
        public:
            output_buffer(std::vector<std::uint8_t> &data, std::size_t limit)
                : m_data(data), m_size(data.size()), m_limit(limit)
            {
            }

            ~output_buffer() { m_data.resize(m_size); }

            output_buffer(const output_buffer &) = delete;
            output_buffer &operator=(const output_buffer &) = delete;

            // Room for `count` more bytes; false past the limit.
            bool reserve(std::size_t count)
            {
                if (count > m_limit || m_size > m_limit - count)
                    return false;
                if (m_size + count > m_data.size())
                {
                    std::size_t grown = std::max<std::size_t>(m_data.size() * 2, 1u << 16);
                    grown = std::clamp(grown, m_size + count, m_limit);
                    m_data.resize(grown);
                }
                return true;
            }

            void put(std::uint8_t byte) noexcept { m_data[m_size++] = byte; }

            // Copies `length` bytes from `distance` back; the ranges may
            // overlap, which is how DEFLATE writes runs.
            void copy(std::size_t distance, std::size_t length) noexcept
            {
                std::uint8_t *out = m_data.data() + m_size;
                const std::uint8_t *from = out - distance;
                if (distance >= length)
                    std::memcpy(out, from, length);
                else
                    for (std::size_t i = 0; i < length; ++i)
                        out[i] = from[i];
                m_size += length;
            }

            std::size_t size() const noexcept { return m_size; }
            std::size_t start() const noexcept { return m_start; }

        private:
            std::vector<std::uint8_t> &m_data;
            std::size_t m_size;
            std::size_t m_start = m_size;
            std::size_t m_limit;
        };

        using failure = std::unexpected<std::string>;

        std::expected<void, std::string> inflate_codes(bit_input &input, output_buffer &output,
                                                       const huffman &literals, const huffman &distances)
        {
            for (;;)
            {
                input.refill();
                const int symbol = literals.decode(input);
                if (input.exhausted())
                    return failure("the compressed data ends in the middle of a block");
                if (symbol < 0)
                    return failure("the compressed data holds an invalid literal/length code");

                if (symbol < 256)
                {
                    if (!output.reserve(1))
                        return failure("the data decompresses to more than the allowed size");
                    output.put(static_cast<std::uint8_t>(symbol));
                    continue;
                }
                if (symbol == 256)
                    return {};
                if (symbol > 285)
                    return failure(std::format("the compressed data uses the invalid length symbol {}", symbol));

                const auto length_symbol = static_cast<std::size_t>(symbol - 257);
                const std::size_t length = length_base[length_symbol] + input.bits(length_extra[length_symbol]);

                if (distances.empty())
                    return failure("the compressed data refers back without a distance code");
                const int distance_symbol = distances.decode(input);
                if (distance_symbol < 0 || distance_symbol > 29)
                    return failure("the compressed data holds an invalid distance code");
                const auto index = static_cast<std::size_t>(distance_symbol);
                const std::size_t distance = distance_base[index] + input.bits(distance_extra[index]);
                if (input.exhausted())
                    return failure("the compressed data ends in the middle of a block");

                if (distance > output.size() - output.start())
                    return failure(std::format("the compressed data refers {} bytes back, before its start", distance));
                if (!output.reserve(length))
                    return failure("the data decompresses to more than the allowed size");
                output.copy(distance, length);
            }
        }

        std::expected<void, std::string> inflate_stored(bit_input &input, output_buffer &output)
        {
            input.align_to_byte();
            const std::uint32_t length = input.bits(16);
            const std::uint32_t complement = input.bits(16);
            if (input.exhausted())
                return failure("the compressed data ends in a block header");
            if ((length ^ 0xFFFFu) != complement)
                return failure("a stored block's length does not match its complement");
            if (!output.reserve(length))
                return failure("the data decompresses to more than the allowed size");
            for (std::uint32_t i = 0; i < length; ++i)
                output.put(static_cast<std::uint8_t>(input.bits(8)));
            if (input.exhausted())
                return failure("the compressed data ends in the middle of a stored block");
            return {};
        }

        std::expected<void, std::string> read_dynamic_codes(bit_input &input, huffman &literals, huffman &distances)
        {
            const std::uint32_t literal_count = input.bits(5) + 257;
            const std::uint32_t distance_count = input.bits(5) + 1;
            const std::uint32_t code_count = input.bits(4) + 4;
            if (literal_count > 286 || distance_count > 30)
                return failure("a block header declares more codes than DEFLATE has");

            static constexpr std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::uint8_t code_lengths[19] = {};
            for (std::uint32_t i = 0; i < code_count; ++i)
                code_lengths[order[i]] = static_cast<std::uint8_t>(input.bits(3));
            if (input.exhausted())
                return failure("the compressed data ends in a block header");

            huffman lengths_code;
            if (!lengths_code.build(code_lengths, 19) || lengths_code.empty())
                return failure("a block header has an invalid code length code");

            std::uint8_t lengths[286 + 30] = {};
            const std::uint32_t total = literal_count + distance_count;
            std::uint32_t at = 0;
            while (at < total)
            {
                input.refill();
                const int symbol = lengths_code.decode(input);
                if (symbol < 0 || input.exhausted())
                    return failure("a block header's code lengths are corrupt");
                if (symbol < 16)
                {
                    lengths[at++] = static_cast<std::uint8_t>(symbol);
                    continue;
                }

                std::uint8_t value = 0;
                std::uint32_t repeat = 0;
                if (symbol == 16)
                {
                    if (at == 0)
                        return failure("a block header repeats a code length before there is one");
                    value = lengths[at - 1];
                    repeat = 3 + input.bits(2);
                }
                else if (symbol == 17)
                    repeat = 3 + input.bits(3);
                else
                    repeat = 11 + input.bits(7);
                if (at + repeat > total)
                    return failure("a block header's code lengths run past their count");
                for (std::uint32_t i = 0; i < repeat; ++i)
                    lengths[at++] = value;
            }

            if (lengths[256] == 0)
                return failure("a block has no end-of-block code");
            if (!literals.build(lengths, literal_count))
                return failure("a block has an invalid literal/length code");
            if (!distances.build(lengths + literal_count, distance_count))
                return failure("a block has an invalid distance code");
            return {};
        }

        const huffman &fixed_literals()
        {
            static const huffman code = [] {
                std::uint8_t lengths[288];
                std::fill(lengths, lengths + 144, std::uint8_t{8});
                std::fill(lengths + 144, lengths + 256, std::uint8_t{9});
                std::fill(lengths + 256, lengths + 280, std::uint8_t{7});
                std::fill(lengths + 280, lengths + 288, std::uint8_t{8});
                huffman result;
                result.build(lengths, 288);
                return result;
            }();
            return code;
        }

        const huffman &fixed_distances()
        {
            static const huffman code = [] {
                std::uint8_t lengths[30];
                std::fill(lengths, lengths + 30, std::uint8_t{5});
                huffman result;
                result.build(lengths, 30, true);
                return result;
            }();
            return code;
        }
    } // namespace

    namespace detail
    {
        std::expected<std::size_t, std::string> inflate_append(std::span<const std::uint8_t> input,
                                                               std::vector<std::uint8_t> &data,
                                                               const inflate_options &options)
        {
            if (options.size_hint != 0)
            {
                // A hint is a guess; DEFLATE's best ratio bounds what the
                // input could possibly produce.
                const std::size_t plausible = input.size() > options.max_output / 1032
                                                  ? options.max_output
                                                  : std::max<std::size_t>(input.size() * 1032, 64);
                data.reserve(data.size() + std::min({options.size_hint, plausible, options.max_output}));
            }

            bit_input bits(input);
            output_buffer output(data, options.max_output);
            auto literals = std::make_unique<huffman>();
            auto distances = std::make_unique<huffman>();

            bool last = false;
            while (!last)
            {
                last = bits.bits(1) != 0;
                const std::uint32_t type = bits.bits(2);
                if (bits.exhausted())
                    return failure(input.empty() ? "the compressed data is empty" : "the compressed data is truncated");

                std::expected<void, std::string> result;
                switch (type)
                {
                case 0:
                    result = inflate_stored(bits, output);
                    break;
                case 1:
                    result = inflate_codes(bits, output, fixed_literals(), fixed_distances());
                    break;
                case 2:
                    result = read_dynamic_codes(bits, *literals, *distances);
                    if (result)
                        result = inflate_codes(bits, output, *literals, *distances);
                    break;
                default:
                    return failure("the compressed data uses the reserved block type 3");
                }
                if (!result)
                    return failure(std::move(result.error()));
            }
            return static_cast<std::size_t>((bits.consumed() + 7) / 8);
        }
    } // namespace detail

    std::expected<std::vector<std::uint8_t>, std::string> inflate(std::span<const std::uint8_t> input,
                                                                  const inflate_options &options)
    {
        std::vector<std::uint8_t> output;
        if (auto result = detail::inflate_append(input, output, options); !result)
            return std::unexpected(std::move(result.error()));
        return output;
    }

} // namespace volt::compress
