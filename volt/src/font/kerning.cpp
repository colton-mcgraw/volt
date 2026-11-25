#include "face.hpp"

#include <algorithm>
#include <bit>

// -------------------------------------------------------------------------
// kerning
// -------------------------------------------------------------------------
//
// Two sources, of which a font may have either or both:
//
// GPOS pair adjustment, the OpenType way and the only one most new fonts
// carry. Taken from every lookup the font's 'kern' feature names, in any
// script: there is no script itemization here, and real fonts register the
// same kerning lookups under all their scripts anyway. Per lookup, the first
// subtable that covers the pair decides (that is what the format means by
// subtables of one lookup); the lookups' values add up. Only the first
// glyph's horizontal advance adjustment is used, which is what kerning is
// for left-to-right text; placement adjustments and device tables are not.
//
// The 'kern' table, format 0, used only when GPOS has no kerning. Both the
// Microsoft (version 0) and Apple (version 1) headers are understood.

namespace volt::font::detail
{

    namespace
    {
        // ---- kern ---------------------------------------------------------

        void parse_kern_table(face &face)
        {
            const bytes kern = face.kern;
            if (kern.size() < 4)
                return;

            const auto take = [&](bytes subtable, std::size_t pairs_header) {
                const std::size_t pairs = pairs_header + 8;
                face.kern_pairs = subtable.from(pairs);
                face.kern_count = static_cast<std::uint32_t>(subtable.fit(pairs, subtable.u16(pairs_header), 6));
            };

            if (kern.u16(0) == 0)
            {
                // Microsoft: u16 version, u16 count; subtables of u16 version,
                // u16 length, u16 coverage with the format in its high byte.
                const std::size_t count = kern.u16(2);
                std::size_t at = 4;
                for (std::size_t i = 0; i < count && kern.contains(at, 6); ++i)
                {
                    const std::uint16_t length = kern.u16(at + 2);
                    const std::uint16_t coverage = kern.u16(at + 4);
                    const bool horizontal = (coverage & 0x1u) != 0;
                    const bool minimum = (coverage & 0x2u) != 0;
                    const bool cross_stream = (coverage & 0x4u) != 0;
                    if ((coverage >> 8) == 0 && horizontal && !minimum && !cross_stream)
                    {
                        take(kern.from(at), 6);
                        return;
                    }
                    if (length < 6)
                        return;
                    at += length;
                }
            }
            else if (kern.u32(0) == 0x00010000u)
            {
                // Apple: u32 version, u32 count; subtables of u32 length, u8
                // coverage (0x80 vertical, 0x40 cross-stream, 0x20 variation),
                // u8 format, u16 tuple index.
                const std::size_t count = kern.u32(4);
                std::size_t at = 8;
                for (std::size_t i = 0; i < count && kern.contains(at, 8); ++i)
                {
                    const std::uint32_t length = kern.u32(at);
                    const std::uint8_t coverage = kern.u8(at + 4);
                    if (kern.u8(at + 5) == 0 && (coverage & 0xE0u) == 0)
                    {
                        take(kern.from(at), 8);
                        return;
                    }
                    if (length < 8)
                        return;
                    at += length;
                }
            }
        }

        int kern_table_value(const face &face, glyph_id left, glyph_id right) noexcept
        {
            const std::uint32_t key = (std::uint32_t{left} << 16) | right;
            std::uint32_t low = 0;
            std::uint32_t high = face.kern_count;
            while (low < high)
            {
                const std::uint32_t middle = low + (high - low) / 2;
                const std::uint32_t pair = face.kern_pairs.u32(std::size_t{middle} * 6);
                if (pair < key)
                    low = middle + 1;
                else if (pair > key)
                    high = middle;
                else
                    return face.kern_pairs.i16(std::size_t{middle} * 6 + 4);
            }
            return 0;
        }

        // ---- GPOS ---------------------------------------------------------

        // The index of `glyph` in a coverage table, or -1.
        int coverage_index(bytes coverage, glyph_id glyph) noexcept
        {
            const std::uint16_t format = coverage.u16(0);
            if (format == 1)
            {
                std::size_t low = 0;
                std::size_t high = coverage.fit(4, coverage.u16(2), 2);
                while (low < high)
                {
                    const std::size_t middle = (low + high) / 2;
                    const std::uint16_t value = coverage.u16(4 + middle * 2);
                    if (value < glyph)
                        low = middle + 1;
                    else if (value > glyph)
                        high = middle;
                    else
                        return static_cast<int>(middle);
                }
            }
            else if (format == 2)
            {
                std::size_t low = 0;
                std::size_t high = coverage.fit(4, coverage.u16(2), 6);
                while (low < high)
                {
                    const std::size_t middle = (low + high) / 2;
                    const std::size_t range = 4 + middle * 6;
                    if (coverage.u16(range + 2) < glyph)
                        low = middle + 1;
                    else if (coverage.u16(range) > glyph)
                        high = middle;
                    else
                        return coverage.u16(range + 4) + (glyph - coverage.u16(range));
                }
            }
            return -1;
        }

        std::uint32_t glyph_class(bytes class_def, glyph_id glyph) noexcept
        {
            const std::uint16_t format = class_def.u16(0);
            if (format == 1)
            {
                const std::uint32_t start = class_def.u16(2);
                const std::uint32_t count = class_def.u16(4);
                if (glyph < start || glyph - start >= count)
                    return 0;
                return class_def.u16(6 + std::size_t{glyph - start} * 2);
            }
            if (format == 2)
            {
                std::size_t low = 0;
                std::size_t high = class_def.fit(4, class_def.u16(2), 6);
                while (low < high)
                {
                    const std::size_t middle = (low + high) / 2;
                    const std::size_t range = 4 + middle * 6;
                    if (class_def.u16(range + 2) < glyph)
                        low = middle + 1;
                    else if (class_def.u16(range) > glyph)
                        high = middle;
                    else
                        return class_def.u16(range + 4);
                }
            }
            return 0;
        }

        // A value record holds one int16 per bit set in its format, in bit
        // order; XAdvance is bit 2.
        std::size_t value_size(std::uint16_t format) noexcept
        {
            return 2u * static_cast<std::size_t>(std::popcount(static_cast<unsigned>(format & 0xFFu)));
        }

        int x_advance(bytes record, std::uint16_t format) noexcept
        {
            if ((format & 0x4u) == 0)
                return 0;
            return record.i16(value_size(format & 0x3u));
        }

        // Whether the subtable covers the pair, and if so its adjustment.
        bool pair_value(bytes subtable, glyph_id left, glyph_id right, int &value) noexcept
        {
            const int covered = coverage_index(subtable.from(subtable.u16(2)), left);
            if (covered < 0)
                return false;
            const std::uint16_t format_1 = subtable.u16(4);
            const std::uint16_t format_2 = subtable.u16(6);
            const std::size_t record_1 = value_size(format_1);
            const std::size_t record_2 = value_size(format_2);

            if (subtable.u16(0) == 1)
            {
                if (static_cast<std::size_t>(covered) >= subtable.u16(8))
                    return false;
                const bytes set = subtable.from(subtable.u16(10 + static_cast<std::size_t>(covered) * 2));
                const std::size_t stride = 2 + record_1 + record_2;
                std::size_t low = 0;
                std::size_t high = set.fit(2, set.u16(0), stride);
                while (low < high)
                {
                    const std::size_t middle = (low + high) / 2;
                    const std::size_t entry = 2 + middle * stride;
                    const std::uint16_t second = set.u16(entry);
                    if (second < right)
                        low = middle + 1;
                    else if (second > right)
                        high = middle;
                    else
                    {
                        value = x_advance(set.from(entry + 2), format_1);
                        return true;
                    }
                }
                return false;
            }

            if (subtable.u16(0) == 2)
            {
                const std::uint32_t class_1 = glyph_class(subtable.from(subtable.u16(8)), left);
                const std::uint32_t class_2 = glyph_class(subtable.from(subtable.u16(10)), right);
                const std::uint32_t count_1 = subtable.u16(12);
                const std::uint32_t count_2 = subtable.u16(14);
                if (class_1 >= count_1 || class_2 >= count_2)
                    return false;
                const std::size_t entry =
                    16 + (std::size_t{class_1} * count_2 + class_2) * (record_1 + record_2);
                value = x_advance(subtable.from(entry), format_1);
                return true;
            }
            return false;
        }

        void parse_gpos(face &face)
        {
            const bytes gpos = face.gpos;
            if (gpos.size() < 10 || gpos.u16(0) != 1)
                return;

            // Every lookup any 'kern' feature uses.
            const bytes features = gpos.from(gpos.u16(6));
            const bytes lookups = gpos.from(gpos.u16(8));
            const std::size_t lookup_count = lookups.fit(2, lookups.u16(0), 2);
            std::vector<std::uint16_t> used;
            const std::size_t feature_count = features.fit(2, features.u16(0), 6);
            for (std::size_t i = 0; i < feature_count; ++i)
            {
                const std::size_t record = 2 + i * 6;
                if (features.u32(record) != tag('k', 'e', 'r', 'n'))
                    continue;
                const bytes feature = features.from(features.u16(record + 4));
                const std::size_t count = feature.fit(4, feature.u16(2), 2);
                for (std::size_t j = 0; j < count; ++j)
                {
                    const std::uint16_t index = feature.u16(4 + j * 2);
                    if (index < lookup_count)
                        used.push_back(index);
                }
            }
            std::sort(used.begin(), used.end());
            used.erase(std::unique(used.begin(), used.end()), used.end());

            // Their pair adjustment subtables, unwrapped from extension
            // subtables (lookup type 9), as offsets from the GPOS table.
            const std::size_t lookups_at = gpos.u16(8);
            for (const std::uint16_t index : used)
            {
                const std::size_t lookup_at = lookups_at + lookups.u16(2 + std::size_t{index} * 2);
                const std::uint16_t type = gpos.u16(lookup_at);
                if (type != 2 && type != 9)
                    continue;
                std::vector<std::uint32_t> pair_subtables;
                const std::size_t count = gpos.fit(lookup_at + 6, gpos.u16(lookup_at + 4), 2);
                for (std::size_t j = 0; j < count; ++j)
                {
                    std::size_t subtable_at = lookup_at + gpos.u16(lookup_at + 6 + j * 2);
                    if (type == 9)
                    {
                        if (gpos.u16(subtable_at) != 1 || gpos.u16(subtable_at + 2) != 2)
                            continue;
                        subtable_at += gpos.u32(subtable_at + 4);
                    }
                    const std::uint16_t format = gpos.u16(subtable_at);
                    if (gpos.contains(subtable_at, 16) && (format == 1 || format == 2))
                        pair_subtables.push_back(static_cast<std::uint32_t>(subtable_at));
                }
                if (!pair_subtables.empty())
                    face.gpos_pair_lookups.push_back(std::move(pair_subtables));
            }
        }
    } // namespace

    void parse_kerning(face &face)
    {
        parse_gpos(face);
        if (face.gpos_pair_lookups.empty())
            parse_kern_table(face);
    }

    int kerning(const face &face, glyph_id left, glyph_id right) noexcept
    {
        if (face.gpos_pair_lookups.empty())
            return face.kern_count != 0 ? kern_table_value(face, left, right) : 0;

        int total = 0;
        for (const std::vector<std::uint32_t> &lookup : face.gpos_pair_lookups)
        {
            for (const std::uint32_t at : lookup)
            {
                int value = 0;
                if (pair_value(face.gpos.from(at), left, right, value))
                {
                    total += value;
                    break;
                }
            }
        }
        return total;
    }

} // namespace volt::font::detail
