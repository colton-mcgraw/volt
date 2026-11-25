#include "face.hpp"

// -------------------------------------------------------------------------
// cmap: code points to glyphs
// -------------------------------------------------------------------------
//
// A font carries several mappings for different platforms and encodings.
// One is chosen at load time, the one that reaches the most of Unicode:
// a full-range format 12 or 13, else a BMP-only format 4 (or 6, 0), else a
// Windows symbol mapping, else as a last resort the old Mac Roman one,
// trusted for ASCII only.

namespace volt::font::detail
{

    namespace
    {
        // How much of Unicode a subtable can reach; 0 for one not to use.
        int rank(std::uint16_t platform, std::uint16_t encoding, std::uint16_t format) noexcept
        {
            const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
            if (unicode)
            {
                if (format == 12 || format == 13)
                    return 5;
                if (format == 4)
                    return 4;
                if (format == 6 || format == 0)
                    return 3;
                return 0;
            }
            if (platform == 3 && encoding == 0 && (format == 4 || format == 12 || format == 6))
                return 2;
            if (platform == 1 && encoding == 0 && (format == 0 || format == 6))
                return 1;
            return 0;
        }

        std::uint32_t lookup_format4(bytes table, std::uint32_t code_point) noexcept
        {
            if (code_point > 0xFFFFu)
                return 0;
            const std::size_t segments = table.u16(6) / 2u;
            const std::size_t ends = 14;
            const std::size_t starts = ends + segments * 2 + 2;
            const std::size_t deltas = starts + segments * 2;
            const std::size_t ranges = deltas + segments * 2;
            if (segments == 0 || !table.contains(ranges, segments * 2))
                return 0;

            // The first segment whose end is at or after the code point.
            std::size_t low = 0;
            std::size_t high = segments;
            while (low < high)
            {
                const std::size_t middle = (low + high) / 2;
                if (table.u16(ends + middle * 2) < code_point)
                    low = middle + 1;
                else
                    high = middle;
            }
            if (low == segments)
                return 0;

            const std::uint32_t start = table.u16(starts + low * 2);
            if (code_point < start)
                return 0;
            const std::uint32_t delta = table.u16(deltas + low * 2);
            const std::size_t range_at = ranges + low * 2;
            const std::uint32_t range = table.u16(range_at);
            if (range == 0)
                return (code_point + delta) & 0xFFFFu;

            // The offset is relative to where it is stored, into the glyph
            // array that follows: the format's famous pointer trick.
            const std::uint32_t glyph = table.u16(range_at + range + (code_point - start) * 2);
            return glyph == 0 ? 0 : (glyph + delta) & 0xFFFFu;
        }

        std::uint32_t lookup_groups(bytes table, std::uint32_t code_point, bool constant) noexcept
        {
            const std::size_t count = table.fit(16, table.u32(12), 12);
            std::size_t low = 0;
            std::size_t high = count;
            while (low < high)
            {
                const std::size_t middle = (low + high) / 2;
                const std::size_t group = 16 + middle * 12;
                if (table.u32(group + 4) < code_point)
                    low = middle + 1;
                else
                    high = middle;
            }
            if (low == count)
                return 0;
            const std::size_t group = 16 + low * 12;
            const std::uint32_t start = table.u32(group);
            if (code_point < start)
                return 0;
            const std::uint32_t first = table.u32(group + 8);
            return constant ? first : first + (code_point - start);
        }

        std::uint32_t lookup(const face &face, std::uint32_t code_point) noexcept
        {
            const bytes table = face.cmap_subtable;
            switch (face.cmap_format)
            {
            case 0:
                return code_point < 256 ? table.u8(6 + code_point) : 0;
            case 4:
                return lookup_format4(table, code_point);
            case 6:
            {
                const std::uint32_t first = table.u16(6);
                const std::uint32_t count = table.u16(8);
                if (code_point < first || code_point - first >= count)
                    return 0;
                return table.u16(10 + (code_point - first) * 2);
            }
            case 12:
                return lookup_groups(table, code_point, false);
            case 13:
                return lookup_groups(table, code_point, true);
            default:
                return 0;
            }
        }

        glyph_id resolve(const face &face, char32_t code_point) noexcept
        {
            std::uint32_t glyph = lookup(face, code_point);
            // Symbol fonts put their characters in the private use area.
            if (glyph == 0 && face.cmap_symbol && code_point < 0x100u)
                glyph = lookup(face, 0xF000u + code_point);
            return glyph < face.glyph_count ? static_cast<glyph_id>(glyph) : glyph_id{0};
        }
    } // namespace

    void parse_cmap(face &face)
    {
        const bytes cmap = face.cmap;
        const std::size_t count = cmap.fit(4, cmap.u16(2), 8);
        int best = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::size_t record = 4 + i * 8;
            const std::uint16_t platform = cmap.u16(record);
            const std::uint16_t encoding = cmap.u16(record + 2);
            const std::uint32_t offset = cmap.u32(record + 4);
            const std::uint16_t format = cmap.u16(offset);

            const int score = rank(platform, encoding, format);
            if (score <= best)
                continue;

            // The subtable's own length field is not used: large format 4
            // tables overflow its 16 bits in real fonts. Every read is
            // bounded by the cmap table instead.
            const bytes table = cmap.from(offset);
            if (table.size() < 8)
                continue;

            best = score;
            face.cmap_subtable = table;
            face.cmap_format = format;
            face.cmap_symbol = platform == 3 && encoding == 0;
        }

        // Mac Roman agrees with Unicode only for ASCII.
        const bool ascii_only = best == 1;
        for (char32_t code_point = 0; code_point < 256; ++code_point)
            face.latin[code_point] = ascii_only && code_point >= 128 ? glyph_id{0} : resolve(face, code_point);
        face.cmap_format = ascii_only ? std::uint16_t{0xFFFF} : face.cmap_format;
    }

    glyph_id map_code_point(const face &face, char32_t code_point) noexcept
    {
        if (code_point < 256)
            return face.latin[code_point];
        return resolve(face, code_point);
    }

} // namespace volt::font::detail
