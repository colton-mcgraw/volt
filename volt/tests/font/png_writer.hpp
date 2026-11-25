#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <vector>

// A grayscale PNG writer for looking at what the font tools render, private
// to them so they need nothing else: the image data is zlib with "stored"
// (uncompressed) deflate blocks, which every PNG reader accepts. Files are
// big; they are scratch output.

namespace volt::tests::font
{
    namespace png_detail
    {
        inline std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0)
        {
            crc = ~crc;
            for (const std::uint8_t byte : bytes)
            {
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return ~crc;
        }

        inline void put32(std::vector<std::uint8_t> &out, std::uint32_t value)
        {
            for (int shift = 24; shift >= 0; shift -= 8)
                out.push_back(static_cast<std::uint8_t>(value >> shift));
        }

        inline void chunk(std::vector<std::uint8_t> &out, const char *type, const std::vector<std::uint8_t> &data)
        {
            put32(out, static_cast<std::uint32_t>(data.size()));
            const std::size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            put32(out, crc32(std::span<const std::uint8_t>(out).subspan(start)));
        }
    } // namespace png_detail

    // `pixels` is width * height bytes, rows top to bottom, 0 black.
    inline bool write_png(const std::string &path, int width, int height, std::span<const std::uint8_t> pixels)
    {
        using namespace png_detail;

        // Each row behind a filter byte of 0 (none).
        std::vector<std::uint8_t> raw;
        raw.reserve(static_cast<std::size_t>(height) * (static_cast<std::size_t>(width) + 1));
        for (int y = 0; y < height; ++y)
        {
            raw.push_back(0);
            const auto row = pixels.subspan(static_cast<std::size_t>(y) * static_cast<std::size_t>(width),
                                            static_cast<std::size_t>(width));
            raw.insert(raw.end(), row.begin(), row.end());
        }

        // zlib: header, stored blocks of at most 65535 bytes, Adler-32.
        std::vector<std::uint8_t> z{0x78, 0x01};
        std::size_t at = 0;
        do
        {
            const std::size_t length = std::min<std::size_t>(raw.size() - at, 65535);
            const bool last = at + length == raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(static_cast<std::uint8_t>(length & 0xFFu));
            z.push_back(static_cast<std::uint8_t>(length >> 8));
            z.push_back(static_cast<std::uint8_t>(~length & 0xFFu));
            z.push_back(static_cast<std::uint8_t>((~length >> 8) & 0xFFu));
            z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at),
                     raw.begin() + static_cast<std::ptrdiff_t>(at + length));
            at += length;
        } while (at < raw.size());
        std::uint32_t a = 1;
        std::uint32_t b = 0;
        for (const std::uint8_t byte : raw)
        {
            a = (a + byte) % 65521u;
            b = (b + a) % 65521u;
        }
        put32(z, (b << 16) | a);

        std::vector<std::uint8_t> file{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        std::vector<std::uint8_t> header;
        put32(header, static_cast<std::uint32_t>(width));
        put32(header, static_cast<std::uint32_t>(height));
        header.insert(header.end(), {8, 0, 0, 0, 0}); // 8-bit grayscale, no interlace
        chunk(file, "IHDR", header);
        chunk(file, "IDAT", z);
        chunk(file, "IEND", {});

        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
        return static_cast<bool>(stream);
    }
} // namespace volt::tests::font
