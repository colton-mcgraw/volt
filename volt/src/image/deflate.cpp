#include <image/compress.hpp>

#include "huffman.hpp"

#include <algorithm>
#include <array>

namespace volt::compress
{

    namespace
    {
        namespace huff = volt::image::detail;

        // ---- bits, least significant first -----------------------------------

        class bit_output
        {
        public:
            explicit bit_output(std::vector<std::uint8_t> &out) : m_out(out) {}

            // `count` up to 32.
            void put(std::uint32_t value, unsigned count)
            {
                m_bits |= std::uint64_t{value} << m_count;
                m_count += count;
                while (m_count >= 8)
                {
                    m_out.push_back(static_cast<std::uint8_t>(m_bits));
                    m_bits >>= 8;
                    m_count -= 8;
                }
            }

            void align()
            {
                if (m_count % 8 != 0)
                    put(0, 8 - m_count % 8);
            }

            // Bits already written past the last whole byte.
            unsigned pending() const noexcept { return m_count; }

        private:
            std::vector<std::uint8_t> &m_out;
            std::uint64_t m_bits = 0;
            unsigned m_count = 0;
        };

        // ---- the symbol alphabets ---------------------------------------------

        constexpr std::uint16_t length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr std::uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr std::uint16_t distance_base[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                     33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr std::uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

        // Symbol index (0 to 28, add 257) for each match length 0 to 258.
        const std::array<std::uint8_t, 259> &length_symbols()
        {
            static const auto table = [] {
                std::array<std::uint8_t, 259> result{};
                for (std::uint8_t code = 0; code < 29; ++code)
                {
                    const unsigned last = code == 28 ? 258u : length_base[code + 1] - 1u;
                    for (unsigned length = length_base[code]; length <= last; ++length)
                        result[length] = code;
                }
                result[258] = 28;
                return result;
            }();
            return table;
        }

        // Symbol for each distance 1 to 32768.
        const std::array<std::uint8_t, 32769> &distance_symbols()
        {
            static const auto table = [] {
                std::array<std::uint8_t, 32769> result{};
                for (std::uint8_t code = 0; code < 30; ++code)
                {
                    const unsigned last = code == 29 ? 32768u : distance_base[code + 1] - 1u;
                    for (unsigned distance = distance_base[code]; distance <= last; ++distance)
                        result[distance] = code;
                }
                return result;
            }();
            return table;
        }

        // A literal (distance 0) or a match.
        struct token
        {
            std::uint16_t value = 0; // the literal byte, or the match length
            std::uint16_t distance = 0;
        };

        // ---- LZ77 ---------------------------------------------------------------

        constexpr std::size_t window = 32768;
        constexpr unsigned hash_bits = 15;
        constexpr std::size_t min_match = 3;
        constexpr std::size_t max_match = 258;

        struct level_settings
        {
            int chain = 0;     // candidates examined per position
            std::size_t nice = 0; // a match this long ends the search
            bool lazy = false; // try the next position before taking a match
        };

        constexpr level_settings levels[10] = {
            {0, 0, false},     {4, 8, false},     {8, 16, false},     {32, 32, false},   {16, 16, true},
            {32, 32, true},    {128, 128, true},  {256, 128, true},   {1024, 258, true}, {4096, 258, true},
        };

        class matcher
        {
        public:
            matcher(std::span<const std::uint8_t> data, const level_settings &settings)
                : m_data(data), m_settings(settings), m_head(std::size_t{1} << hash_bits, -1), m_previous(window, -1)
            {
            }

            struct match
            {
                std::size_t length = 0;
                std::size_t distance = 0;
            };

            // The longest earlier match for the bytes at `at`, among what
            // insert() has seen.
            match find(std::size_t at) const noexcept
            {
                match best;
                const std::size_t available = std::min(max_match, m_data.size() - at);
                if (available < min_match)
                    return best;

                const std::uint8_t *here = m_data.data() + at;
                std::int64_t candidate = m_head[hash(at)];
                int chain = m_settings.chain;
                std::size_t best_length = min_match - 1;
                while (candidate >= 0 && chain-- > 0)
                {
                    const auto from = static_cast<std::size_t>(candidate);
                    if (at - from > window)
                        break;
                    const std::uint8_t *there = m_data.data() + from;
                    if (there[best_length] == here[best_length] && there[0] == here[0] && there[1] == here[1])
                    {
                        std::size_t length = 2;
                        while (length < available && there[length] == here[length])
                            ++length;
                        if (length > best_length)
                        {
                            best_length = length;
                            best = {length, at - from};
                            if (length >= m_settings.nice || length == available)
                                break;
                        }
                    }
                    const std::int64_t next = m_previous[from % window];
                    if (next >= candidate)
                        break; // overwritten by a newer position: the chain ends
                    candidate = next;
                }

                // A short match far back costs more than its literals.
                if (best.length == min_match && best.distance > 4096)
                    return {};
                return best;
            }

            void insert(std::size_t at) noexcept
            {
                if (at + min_match > m_data.size())
                    return;
                const std::size_t h = hash(at);
                m_previous[at % window] = m_head[h];
                m_head[h] = static_cast<std::int64_t>(at);
            }

        private:
            std::size_t hash(std::size_t at) const noexcept
            {
                const std::uint32_t key = std::uint32_t{m_data[at]} << 16 | std::uint32_t{m_data[at + 1]} << 8 |
                                          std::uint32_t{m_data[at + 2]};
                return (key * 0x9E3779B1u) >> (32 - hash_bits);
            }

            std::span<const std::uint8_t> m_data;
            level_settings m_settings;
            std::vector<std::int64_t> m_head;
            std::vector<std::int64_t> m_previous;
        };

        // ---- blocks ---------------------------------------------------------------

        struct code
        {
            std::vector<std::uint8_t> lengths;
            std::vector<std::uint16_t> codes; // bit-reversed, ready to write
        };

        code make_code(std::vector<std::uint8_t> lengths)
        {
            code result;
            result.codes = huff::canonical_codes(lengths);
            for (std::size_t s = 0; s < lengths.size(); ++s)
                result.codes[s] = static_cast<std::uint16_t>(huff::reverse_bits(result.codes[s], lengths[s]));
            result.lengths = std::move(lengths);
            return result;
        }

        const code &fixed_literal_code()
        {
            static const code result = [] {
                std::vector<std::uint8_t> lengths(288);
                std::fill(lengths.begin(), lengths.begin() + 144, std::uint8_t{8});
                std::fill(lengths.begin() + 144, lengths.begin() + 256, std::uint8_t{9});
                std::fill(lengths.begin() + 256, lengths.begin() + 280, std::uint8_t{7});
                std::fill(lengths.begin() + 280, lengths.end(), std::uint8_t{8});
                return make_code(std::move(lengths));
            }();
            return result;
        }

        const code &fixed_distance_code()
        {
            static const code result = make_code(std::vector<std::uint8_t>(30, 5));
            return result;
        }

        // A code where at least two symbols have lengths, so that every
        // decoder takes it (a lone code of length 1 is legal but incomplete).
        void make_decodable(std::vector<std::uint8_t> &lengths)
        {
            std::size_t used = 0;
            for (const std::uint8_t length : lengths)
                used += length != 0;
            for (std::size_t s = 0; s < lengths.size() && used < 2; ++s)
                if (lengths[s] == 0)
                {
                    lengths[s] = 1;
                    ++used;
                }
        }

        // The code lengths of a dynamic block header, run-length coded with
        // symbols 16 (repeat previous), 17 and 18 (runs of zeros).
        struct length_run
        {
            std::uint8_t symbol = 0;
            std::uint8_t extra = 0;
        };

        std::vector<length_run> run_length(std::span<const std::uint8_t> lengths)
        {
            std::vector<length_run> runs;
            std::size_t i = 0;
            while (i < lengths.size())
            {
                const std::uint8_t value = lengths[i];
                std::size_t run = 1;
                while (i + run < lengths.size() && lengths[i + run] == value)
                    ++run;
                i += run;

                if (value == 0)
                {
                    while (run >= 11)
                    {
                        const std::size_t take = std::min<std::size_t>(run, 138);
                        runs.push_back({18, static_cast<std::uint8_t>(take - 11)});
                        run -= take;
                    }
                    if (run >= 3)
                    {
                        runs.push_back({17, static_cast<std::uint8_t>(run - 3)});
                        run = 0;
                    }
                    for (; run > 0; --run)
                        runs.push_back({0, 0});
                    continue;
                }

                runs.push_back({value, 0});
                --run;
                while (run >= 3)
                {
                    const std::size_t take = std::min<std::size_t>(run, 6);
                    runs.push_back({16, static_cast<std::uint8_t>(take - 3)});
                    run -= take;
                }
                for (; run > 0; --run)
                    runs.push_back({value, 0});
            }
            return runs;
        }

        constexpr std::uint8_t length_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

        class block_writer
        {
        public:
            explicit block_writer(std::vector<std::uint8_t> &out) : m_bits(out) {}

            // One block of tokens, which together stand for `raw`.
            void write(std::span<const token> tokens, std::span<const std::uint8_t> raw, bool final)
            {
                const auto &length_symbol = length_symbols();
                const auto &distance_symbol = distance_symbols();

                std::vector<std::uint32_t> literal_frequency(286, 0);
                std::vector<std::uint32_t> distance_frequency(30, 0);
                std::uint64_t extra_bits = 0;
                for (const token &t : tokens)
                {
                    if (t.distance == 0)
                    {
                        ++literal_frequency[t.value];
                        continue;
                    }
                    const std::uint8_t l = length_symbol[t.value];
                    const std::uint8_t d = distance_symbol[t.distance];
                    ++literal_frequency[257u + l];
                    ++distance_frequency[d];
                    extra_bits += length_extra[l] + distance_extra[d];
                }
                literal_frequency[256] = 1;

                // The dynamic code and what its header costs.
                std::vector<std::uint8_t> literal_lengths = huff::huffman_lengths(literal_frequency, 15);
                std::vector<std::uint8_t> distance_lengths = huff::huffman_lengths(distance_frequency, 15);
                make_decodable(literal_lengths);
                make_decodable(distance_lengths);

                std::size_t literal_count = 286;
                while (literal_count > 257 && literal_lengths[literal_count - 1] == 0)
                    --literal_count;
                std::size_t distance_count = 30;
                while (distance_count > 1 && distance_lengths[distance_count - 1] == 0)
                    --distance_count;

                std::vector<std::uint8_t> all(literal_lengths.begin(),
                                              literal_lengths.begin() + static_cast<std::ptrdiff_t>(literal_count));
                all.insert(all.end(), distance_lengths.begin(),
                           distance_lengths.begin() + static_cast<std::ptrdiff_t>(distance_count));
                const std::vector<length_run> runs = run_length(all);

                std::vector<std::uint32_t> run_frequency(19, 0);
                for (const length_run &run : runs)
                    ++run_frequency[run.symbol];
                const std::vector<std::uint8_t> run_lengths = huff::huffman_lengths(run_frequency, 7);
                std::size_t run_code_count = 19;
                while (run_code_count > 4 && run_lengths[length_order[run_code_count - 1]] == 0)
                    --run_code_count;

                std::uint64_t dynamic_cost = 3 + 5 + 5 + 4 + 3 * run_code_count + extra_bits;
                for (const length_run &run : runs)
                    dynamic_cost += run_lengths[run.symbol] + (run.symbol == 16 ? 2u : run.symbol == 17 ? 3u
                                                                                       : run.symbol == 18 ? 7u
                                                                                                          : 0u);
                std::uint64_t fixed_cost = 3 + extra_bits;
                const code &fixed_literals = fixed_literal_code();
                for (std::size_t s = 0; s < 286; ++s)
                {
                    dynamic_cost += std::uint64_t{literal_frequency[s]} * literal_lengths[s];
                    fixed_cost += std::uint64_t{literal_frequency[s]} * fixed_literals.lengths[s];
                }
                for (std::size_t s = 0; s < 30; ++s)
                {
                    dynamic_cost += std::uint64_t{distance_frequency[s]} * distance_lengths[s];
                    fixed_cost += std::uint64_t{distance_frequency[s]} * 5u;
                }

                // Stored: header, padding to a byte, then a length pair per
                // 65535 bytes.
                const std::uint64_t stored_blocks = std::max<std::uint64_t>(1, (raw.size() + 65534) / 65535);
                const std::uint64_t stored_cost =
                    stored_blocks * (3 + 7 + 32) + 8 * std::uint64_t{raw.size()};

                if (stored_cost <= dynamic_cost && stored_cost <= fixed_cost)
                {
                    write_stored(raw, final);
                    return;
                }

                m_bits.put(final ? 1u : 0u, 1);
                if (fixed_cost <= dynamic_cost)
                {
                    m_bits.put(1, 2);
                    write_tokens(tokens, fixed_literals, fixed_distance_code());
                    return;
                }

                m_bits.put(2, 2);
                m_bits.put(static_cast<std::uint32_t>(literal_count - 257), 5);
                m_bits.put(static_cast<std::uint32_t>(distance_count - 1), 5);
                m_bits.put(static_cast<std::uint32_t>(run_code_count - 4), 4);
                for (std::size_t i = 0; i < run_code_count; ++i)
                    m_bits.put(run_lengths[length_order[i]], 3);

                const code run_code = make_code(run_lengths);
                for (const length_run &run : runs)
                {
                    m_bits.put(run_code.codes[run.symbol], run_code.lengths[run.symbol]);
                    if (run.symbol == 16)
                        m_bits.put(run.extra, 2);
                    else if (run.symbol == 17)
                        m_bits.put(run.extra, 3);
                    else if (run.symbol == 18)
                        m_bits.put(run.extra, 7);
                }
                write_tokens(tokens, make_code(std::move(literal_lengths)), make_code(std::move(distance_lengths)));
            }

            void write_stored(std::span<const std::uint8_t> raw, bool final)
            {
                std::size_t at = 0;
                do
                {
                    const std::size_t take = std::min<std::size_t>(raw.size() - at, 65535);
                    const bool last = at + take == raw.size();
                    m_bits.put(final && last ? 1u : 0u, 1);
                    m_bits.put(0, 2);
                    m_bits.align();
                    m_bits.put(static_cast<std::uint32_t>(take), 16);
                    m_bits.put(static_cast<std::uint32_t>(take ^ 0xFFFFu), 16);
                    for (std::size_t i = 0; i < take; ++i)
                        m_bits.put(raw[at + i], 8);
                    at += take;
                } while (at < raw.size());
            }

            void finish() { m_bits.align(); }

        private:
            void write_tokens(std::span<const token> tokens, const code &literals, const code &distances)
            {
                const auto &length_symbol = length_symbols();
                const auto &distance_symbol = distance_symbols();
                for (const token &t : tokens)
                {
                    if (t.distance == 0)
                    {
                        m_bits.put(literals.codes[t.value], literals.lengths[t.value]);
                        continue;
                    }
                    const std::uint8_t l = length_symbol[t.value];
                    const std::size_t symbol = 257u + l;
                    m_bits.put(literals.codes[symbol], literals.lengths[symbol]);
                    m_bits.put(t.value - length_base[l], length_extra[l]);
                    const std::uint8_t d = distance_symbol[t.distance];
                    m_bits.put(distances.codes[d], distances.lengths[d]);
                    m_bits.put(t.distance - distance_base[d], distance_extra[d]);
                }
                m_bits.put(literals.codes[256], literals.lengths[256]);
            }

            bit_output m_bits;
        };
    } // namespace

    std::vector<std::uint8_t> deflate(std::span<const std::uint8_t> input, int level)
    {
        level = std::clamp(level, 0, 9);
        std::vector<std::uint8_t> out;
        out.reserve(input.size() / 2 + 64);
        block_writer writer(out);

        if (level == 0 || input.empty())
        {
            writer.write_stored(input, true);
            writer.finish();
            return out;
        }

        const level_settings &settings = levels[level];
        matcher lz(input, settings);

        // Blocks of this many tokens: long enough to amortize a header, short
        // enough for the code to follow changes in the data.
        constexpr std::size_t block_tokens = 1u << 15;
        std::vector<token> tokens;
        tokens.reserve(block_tokens);
        std::size_t block_start = 0;

        std::size_t at = 0;
        while (at < input.size())
        {
            matcher::match found = lz.find(at);
            lz.insert(at);

            if (settings.lazy && found.length >= min_match && found.length < settings.nice && at + 1 < input.size())
            {
                // A longer match one byte on is worth a literal now.
                const matcher::match next = lz.find(at + 1);
                if (next.length > found.length)
                    found = {};
            }

            if (found.length >= min_match)
            {
                tokens.push_back({static_cast<std::uint16_t>(found.length), static_cast<std::uint16_t>(found.distance)});
                for (std::size_t i = 1; i < found.length; ++i)
                    lz.insert(at + i);
                at += found.length;
            }
            else
            {
                tokens.push_back({input[at], 0});
                ++at;
            }

            if (tokens.size() >= block_tokens || at >= input.size())
            {
                const bool final = at >= input.size();
                writer.write(tokens, input.subspan(block_start, at - block_start), final);
                tokens.clear();
                block_start = at;
            }
        }
        writer.finish();
        return out;
    }

} // namespace volt::compress
