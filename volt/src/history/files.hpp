#pragma once

#include <json/json.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

// Private to the history module: UTF-8 paths, case folding, and files
// written so that a crash leaves the old file or the new one.

namespace volt::history::detail
{

    // ---- UTF-8 -------------------------------------------------------------------

    // Decodes the code point at text[at], advancing `at`; -1 for a sequence
    // that is not valid UTF-8 (overlong, a surrogate, beyond U+10FFFF,
    // truncated).
    inline std::int32_t decode_utf8(std::string_view text, std::size_t &at) noexcept
    {
        const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
        const std::uint8_t first = byte(at);
        if (first < 0x80)
        {
            ++at;
            return first;
        }
        std::size_t length = 0;
        std::int32_t code = 0;
        std::int32_t minimum = 0;
        if ((first & 0xE0) == 0xC0)
        {
            length = 2;
            code = first & 0x1F;
            minimum = 0x80;
        }
        else if ((first & 0xF0) == 0xE0)
        {
            length = 3;
            code = first & 0x0F;
            minimum = 0x800;
        }
        else if ((first & 0xF8) == 0xF0)
        {
            length = 4;
            code = first & 0x07;
            minimum = 0x10000;
        }
        else
            return -1;
        if (at + length > text.size())
            return -1;
        for (std::size_t i = 1; i < length; ++i)
        {
            const std::uint8_t next = byte(at + i);
            if ((next & 0xC0) != 0x80)
                return -1;
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return -1;
        at += length;
        return code;
    }

    inline bool valid_utf8(std::string_view text) noexcept
    {
        for (std::size_t at = 0; at < text.size();)
            if (decode_utf8(text, at) < 0)
                return false;
        return true;
    }

    inline void encode_utf8(std::int32_t code, std::string &out)
    {
        const auto put = [&](std::int32_t b) { out += static_cast<char>(static_cast<std::uint8_t>(b)); };
        if (code < 0x80)
            put(code);
        else if (code < 0x800)
        {
            put(0xC0 | (code >> 6));
            put(0x80 | (code & 0x3F));
        }
        else if (code < 0x10000)
        {
            put(0xE0 | (code >> 12));
            put(0x80 | ((code >> 6) & 0x3F));
            put(0x80 | (code & 0x3F));
        }
        else
        {
            put(0xF0 | (code >> 18));
            put(0x80 | ((code >> 12) & 0x3F));
            put(0x80 | ((code >> 6) & 0x3F));
            put(0x80 | (code & 0x3F));
        }
    }

    // The lower case of a code point, for the scripts file names are most
    // often written in: ASCII, Latin-1, Latin Extended-A, Greek and
    // Cyrillic. Others are returned as they are.
    inline std::int32_t lower_case(std::int32_t c) noexcept
    {
        if (c >= 'A' && c <= 'Z')
            return c + 32;
        if (c < 0xC0)
            return c;
        if (c <= 0xDE)
            return c == 0xD7 ? c : c + 32; // not the multiplication sign
        if (c >= 0x100 && c <= 0x17F)
        {
            if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F)
                return c; // dotted and dotless i, kra, 'n, long s: no simple pair
            if (c == 0x178)
                return 0xFF;
            const bool odd_upper = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
            if (odd_upper)
                return c % 2 == 1 ? c + 1 : c;
            return c % 2 == 0 ? c + 1 : c;
        }
        if (c >= 0x391 && c <= 0x3AB && c != 0x3A2)
            return c + 32;
        if (c >= 0x410 && c <= 0x42F)
            return c + 32;
        if (c >= 0x400 && c <= 0x40F)
            return c + 80;
        return c;
    }

    // The text with its letters in lower case (lower_case), for telling
    // apart names that a case-insensitive file system takes for one.
    // Precondition: valid UTF-8.
    inline std::string fold_case(std::string_view text)
    {
        std::string folded;
        folded.reserve(text.size());
        for (std::size_t at = 0; at < text.size();)
        {
            const std::int32_t c = decode_utf8(text, at);
            if (c < 0)
                return std::string(text);
            encode_utf8(lower_case(c), folded);
        }
        return folded;
    }

    // ---- paths -------------------------------------------------------------------

    // A path from UTF-8 text, and back as UTF-8 with forward slashes. A
    // std::filesystem::path made from a std::string reads it in the
    // system's narrow encoding, which on Windows is not UTF-8. The text
    // must be valid UTF-8: the conversion throws on Windows otherwise, so
    // invalid text gives an empty path here.
    inline std::filesystem::path path_from_utf8(std::string_view text)
    {
        if (!valid_utf8(text))
            return {};
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    // Empty for a path that is not representable (a name with an unpaired
    // UTF-16 surrogate on Windows).
    inline std::string path_to_utf8(const std::filesystem::path &path)
    {
        try
        {
            const std::u8string text = path.generic_u8string();
            return std::string(text.begin(), text.end());
        }
        catch (const std::exception &)
        {
            return {};
        }
    }

    // Whether `inner` is `base` or below it, comparing whole components
    // of the two paths as given (make them canonical first).
    inline bool is_within(const std::filesystem::path &base, const std::filesystem::path &inner)
    {
        auto b = base.begin();
        auto i = inner.begin();
        for (; b != base.end(); ++b, ++i)
        {
            if (b->empty() && std::next(b) == base.end())
                break; // a trailing separator
            if (i == inner.end() || *b != *i)
                return false;
        }
        return true;
    }

    // ---- files -------------------------------------------------------------------

    // Whether the file there is a regular file holding exactly these bytes.
    inline bool file_holds(const std::filesystem::path &file, std::string_view bytes)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error))
            return false;
        const std::uintmax_t size = std::filesystem::file_size(file, error);
        if (error || size != bytes.size())
            return false;
        const auto existing = json::read_text_file(file, size);
        return existing && *existing == bytes;
    }

    // Replaces the file through a temporary file next to it and a rename,
    // so that a failure leaves the old file or the new one. Whatever is at
    // the temporary file's name (a leftover, or a link put there) is
    // removed first rather than written through.
    inline std::expected<void, std::string> write_atomically(const std::filesystem::path &file, std::string_view bytes)
    {
        const std::string shown = path_to_utf8(file);
        std::filesystem::path temporary = file;
        temporary += ".partial";
        std::error_code error;
        std::filesystem::remove(temporary, error);
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out)
                return std::unexpected(std::format("{}: cannot create the file", shown));
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.flush();
            if (!out)
            {
                out.close();
                std::filesystem::remove(temporary, error);
                return std::unexpected(std::format("{}: writing the file failed", shown));
            }
        }
        std::filesystem::rename(temporary, file, error);
        if (error)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return std::unexpected(std::format("{}: cannot replace the file ({})", shown, error.message()));
        }
        return {};
    }

} // namespace volt::history::detail
