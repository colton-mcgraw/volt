#include <font/utf8.hpp>

namespace volt::font
{

    namespace
    {
        std::uint8_t byte_at(std::string_view text, std::size_t offset) noexcept
        {
            return static_cast<std::uint8_t>(text[offset]);
        }

        bool continuation(std::uint8_t byte) noexcept
        {
            return (byte & 0xC0u) == 0x80u;
        }
    } // namespace

    decoded decode_utf8(std::string_view text, std::size_t offset) noexcept
    {
        if (offset >= text.size())
            return {};

        const std::uint8_t lead = byte_at(text, offset);
        if (lead < 0x80u)
            return {lead, 1};

        // Table 3-7 of the Unicode standard: the length of the sequence a
        // lead byte starts, and the range its second byte must fall in. The
        // narrowed ranges after E0, ED, F0 and F4 are what rule out overlong
        // forms, surrogates and values past U+10FFFF.
        std::uint32_t length = 0;
        std::uint8_t low = 0x80u;
        std::uint8_t high = 0xBFu;
        char32_t value = 0;
        if (lead >= 0xC2u && lead <= 0xDFu)
        {
            length = 2;
            value = lead & 0x1Fu;
        }
        else if (lead >= 0xE0u && lead <= 0xEFu)
        {
            length = 3;
            value = lead & 0x0Fu;
            if (lead == 0xE0u)
                low = 0xA0u;
            else if (lead == 0xEDu)
                high = 0x9Fu;
        }
        else if (lead >= 0xF0u && lead <= 0xF4u)
        {
            length = 4;
            value = lead & 0x07u;
            if (lead == 0xF0u)
                low = 0x90u;
            else if (lead == 0xF4u)
                high = 0x8Fu;
        }
        else
        {
            // A stray continuation byte, or a lead byte that is never valid.
            return {replacement_character, 1};
        }

        for (std::uint32_t i = 1; i < length; ++i)
        {
            if (offset + i >= text.size())
                return {replacement_character, i};
            const std::uint8_t next = byte_at(text, offset + i);
            const bool fits = i == 1 ? (next >= low && next <= high) : continuation(next);
            if (!fits)
                return {replacement_character, i}; // the maximal subpart ends before `next`
            value = (value << 6) | (next & 0x3Fu);
        }
        return {value, length};
    }

    void append_utf8(std::string &out, char32_t code_point)
    {
        if (code_point > 0x10FFFFu || (code_point >= 0xD800u && code_point <= 0xDFFFu))
            code_point = replacement_character;

        const auto put = [&](std::uint32_t byte) { out.push_back(static_cast<char>(static_cast<std::uint8_t>(byte))); };
        if (code_point < 0x80u)
        {
            put(code_point);
        }
        else if (code_point < 0x800u)
        {
            put(0xC0u | (code_point >> 6));
            put(0x80u | (code_point & 0x3Fu));
        }
        else if (code_point < 0x10000u)
        {
            put(0xE0u | (code_point >> 12));
            put(0x80u | ((code_point >> 6) & 0x3Fu));
            put(0x80u | (code_point & 0x3Fu));
        }
        else
        {
            put(0xF0u | (code_point >> 18));
            put(0x80u | ((code_point >> 12) & 0x3Fu));
            put(0x80u | ((code_point >> 6) & 0x3Fu));
            put(0x80u | (code_point & 0x3Fu));
        }
    }

    std::size_t next_boundary(std::string_view text, std::size_t offset) noexcept
    {
        if (offset >= text.size())
            return text.size();
        return offset + decode_utf8(text, offset).length;
    }

    std::size_t previous_boundary(std::string_view text, std::size_t offset) noexcept
    {
        if (offset > text.size())
            offset = text.size();
        if (offset == 0)
            return 0;

        // A byte that is not a continuation byte always starts something,
        // because decoding never takes one as part of the sequence before
        // it. No sequence is longer than four bytes, so if there is none in
        // the last four, the byte just before is a stray of its own.
        const std::size_t limit = offset >= 4 ? offset - 4 : 0;
        std::size_t start = offset;
        for (std::size_t i = offset; i-- > limit;)
        {
            if (!continuation(byte_at(text, i)))
            {
                start = i;
                break;
            }
        }
        if (start == offset)
            return offset - 1;

        // Decode forward from there, as decoding from the beginning would.
        std::size_t position = start;
        for (;;)
        {
            const std::size_t next = position + decode_utf8(text, position).length;
            if (next >= offset || next == position)
                return position;
            position = next;
        }
    }

    std::size_t count_code_points(std::string_view text) noexcept
    {
        std::size_t count = 0;
        for (std::size_t offset = 0; offset < text.size(); offset += decode_utf8(text, offset).length)
            ++count;
        return count;
    }

} // namespace volt::font
