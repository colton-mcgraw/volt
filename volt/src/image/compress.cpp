#include <image/compress.hpp>

#include "inflate.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace volt::compress
{

    // ---- checksums ------------------------------------------------------------

    namespace
    {
        // Slicing by four: four table lookups per four bytes instead of a
        // dependent lookup per byte.
        const std::array<std::array<std::uint32_t, 256>, 4> &crc_tables()
        {
            static const auto tables = [] {
                std::array<std::array<std::uint32_t, 256>, 4> result{};
                for (std::uint32_t n = 0; n < 256; ++n)
                {
                    std::uint32_t c = n;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    result[0][n] = c;
                }
                for (std::uint32_t n = 0; n < 256; ++n)
                    for (std::size_t t = 1; t < 4; ++t)
                        result[t][n] = (result[t - 1][n] >> 8) ^ result[0][result[t - 1][n] & 0xFFu];
                return result;
            }();
            return tables;
        }
    } // namespace

    std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) noexcept
    {
        const auto &table = crc_tables();
        std::uint32_t c = ~crc;
        std::size_t i = 0;
        for (; i + 4 <= data.size(); i += 4)
        {
            c ^= std::uint32_t{data[i]} | std::uint32_t{data[i + 1]} << 8 | std::uint32_t{data[i + 2]} << 16 |
                 std::uint32_t{data[i + 3]} << 24;
            c = table[3][c & 0xFFu] ^ table[2][(c >> 8) & 0xFFu] ^ table[1][(c >> 16) & 0xFFu] ^ table[0][c >> 24];
        }
        for (; i < data.size(); ++i)
            c = table[0][(c ^ data[i]) & 0xFFu] ^ (c >> 8);
        return ~c;
    }

    std::uint32_t adler32(std::span<const std::uint8_t> data, std::uint32_t adler) noexcept
    {
        constexpr std::uint32_t modulus = 65521;
        // The most bytes that can be summed before the 32-bit sums could
        // overflow, as zlib works it out.
        constexpr std::size_t run = 5552;

        std::uint32_t a = adler & 0xFFFFu;
        std::uint32_t b = adler >> 16;
        std::size_t i = 0;
        while (i < data.size())
        {
            const std::size_t end = std::min(data.size(), i + run);
            for (; i < end; ++i)
            {
                a += data[i];
                b += a;
            }
            a %= modulus;
            b %= modulus;
        }
        return (b << 16) | a;
    }

    // ---- zlib -----------------------------------------------------------------

    namespace
    {
        std::uint32_t read_be32(std::span<const std::uint8_t> data, std::size_t at)
        {
            return std::uint32_t{data[at]} << 24 | std::uint32_t{data[at + 1]} << 16 | std::uint32_t{data[at + 2]} << 8 |
                   std::uint32_t{data[at + 3]};
        }

        std::uint32_t read_le32(std::span<const std::uint8_t> data, std::size_t at)
        {
            return std::uint32_t{data[at]} | std::uint32_t{data[at + 1]} << 8 | std::uint32_t{data[at + 2]} << 16 |
                   std::uint32_t{data[at + 3]} << 24;
        }

        void put_be32(std::vector<std::uint8_t> &out, std::uint32_t value)
        {
            for (int shift = 24; shift >= 0; shift -= 8)
                out.push_back(static_cast<std::uint8_t>(value >> shift));
        }

        void put_le32(std::vector<std::uint8_t> &out, std::uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
                out.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    } // namespace

    std::expected<std::vector<std::uint8_t>, std::string> zlib_decompress(std::span<const std::uint8_t> input,
                                                                          const inflate_options &options)
    {
        if (input.size() < 2)
            return std::unexpected(std::string("the zlib stream is too short to hold a header"));
        const std::uint8_t method = input[0];
        const std::uint8_t flags = input[1];
        if ((method & 0x0Fu) != 8)
            return std::unexpected(std::format("the zlib stream uses compression method {}, not DEFLATE", method & 0x0Fu));
        if ((method >> 4) > 7)
            return std::unexpected(std::string("the zlib stream declares a window larger than 32 KiB"));
        if ((method * 256u + flags) % 31u != 0)
            return std::unexpected(std::string("the zlib stream's header check fails"));
        if (flags & 0x20u)
            return std::unexpected(std::string("the zlib stream needs a preset dictionary"));

        std::vector<std::uint8_t> output;
        auto consumed = detail::inflate_append(input.subspan(2), output, options);
        if (!consumed)
            return std::unexpected(std::move(consumed.error()));

        const std::size_t trailer = 2 + *consumed;
        if (input.size() < trailer + 4)
            return std::unexpected(std::string("the zlib stream ends before its checksum"));
        if (read_be32(input, trailer) != adler32(output))
            return std::unexpected(std::string("the zlib stream's Adler-32 checksum does not match its data"));
        return output;
    }

    std::vector<std::uint8_t> zlib_compress(std::span<const std::uint8_t> input, int level)
    {
        level = std::clamp(level, 0, 9);
        const std::uint32_t speed = level < 2 ? 0u : level < 6 ? 1u : level == 6 ? 2u : 3u;
        const std::uint32_t method = 0x78; // DEFLATE, 32 KiB window
        std::uint32_t flags = speed << 6;
        flags += (31u - (method * 256u + flags) % 31u) % 31u;

        std::vector<std::uint8_t> out{static_cast<std::uint8_t>(method), static_cast<std::uint8_t>(flags)};
        const std::vector<std::uint8_t> body = deflate(input, level);
        out.insert(out.end(), body.begin(), body.end());
        put_be32(out, adler32(input));
        return out;
    }

    // ---- gzip -----------------------------------------------------------------

    bool is_gzip(std::span<const std::uint8_t> input) noexcept
    {
        return input.size() >= 3 && input[0] == 0x1F && input[1] == 0x8B && input[2] == 8;
    }

    std::expected<std::vector<std::uint8_t>, std::string> gzip_decompress(std::span<const std::uint8_t> input,
                                                                          const inflate_options &options)
    {
        if (!is_gzip(input))
            return std::unexpected(std::string("the data is not a gzip file"));

        std::vector<std::uint8_t> output;
        std::size_t at = 0;
        while (at < input.size() && is_gzip(input.subspan(at)))
        {
            const std::span<const std::uint8_t> member = input.subspan(at);
            if (member.size() < 10)
                return std::unexpected(std::string("the gzip file ends in a header"));
            const std::uint8_t flags = member[3];
            if (flags & 0xE0u)
                return std::unexpected(std::string("the gzip header sets reserved flags"));

            std::size_t position = 10;
            if (flags & 0x04u) // FEXTRA
            {
                if (member.size() < position + 2)
                    return std::unexpected(std::string("the gzip file ends in a header"));
                position += 2 + (std::size_t{member[position]} | std::size_t{member[position + 1]} << 8);
            }
            for (const std::uint8_t field : {std::uint8_t{0x08}, std::uint8_t{0x10}}) // FNAME, FCOMMENT
            {
                if (!(flags & field))
                    continue;
                while (position < member.size() && member[position] != 0)
                    ++position;
                ++position;
            }
            if (flags & 0x02u) // FHCRC
                position += 2;
            if (position >= member.size())
                return std::unexpected(std::string("the gzip file ends in a header"));

            const std::size_t start = output.size();
            auto consumed = detail::inflate_append(member.subspan(position), output, options);
            if (!consumed)
                return std::unexpected(std::move(consumed.error()));
            position += *consumed;
            if (member.size() < position + 8)
                return std::unexpected(std::string("the gzip file ends before its checksum"));

            const std::span<const std::uint8_t> produced(output.data() + start, output.size() - start);
            if (read_le32(member, position) != crc32(produced))
                return std::unexpected(std::string("the gzip file's CRC-32 does not match its data"));
            if (read_le32(member, position + 4) != static_cast<std::uint32_t>(produced.size()))
                return std::unexpected(std::string("the gzip file's length does not match its data"));
            at += position + 8;
        }
        return output;
    }

    std::vector<std::uint8_t> gzip_compress(std::span<const std::uint8_t> input, int level)
    {
        level = std::clamp(level, 0, 9);
        // No name, no time stamp: the same input gives the same file.
        std::vector<std::uint8_t> out{0x1F, 0x8B, 8, 0, 0, 0, 0, 0, static_cast<std::uint8_t>(level == 9 ? 2 : level == 1 ? 4 : 0),
                                      255};
        const std::vector<std::uint8_t> body = deflate(input, level);
        out.insert(out.end(), body.begin(), body.end());
        put_le32(out, crc32(input));
        put_le32(out, static_cast<std::uint32_t>(input.size()));
        return out;
    }

} // namespace volt::compress
