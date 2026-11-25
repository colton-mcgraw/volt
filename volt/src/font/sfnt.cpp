#include "face.hpp"

#include <font/utf8.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>

namespace volt::font
{

    namespace detail
    {
        namespace
        {
            // Fonts are a few megabytes, the largest CJK collections a few
            // dozen. Anything far beyond that is not a font.
            constexpr std::uintmax_t max_file_size = std::uintmax_t{512} << 20;

            std::atomic<std::uint64_t> next_id{1};

            constexpr std::uint32_t collection_tag = tag('t', 't', 'c', 'f');

            std::uint32_t collection_count(bytes file) noexcept
            {
                if (file.u32(0) != collection_tag)
                    return 1;
                // Each face needs a 4-byte offset after the 12-byte header.
                return static_cast<std::uint32_t>(file.fit(12, file.u32(8), 4));
            }

            // ---- name -----------------------------------------------------

            std::string decode_utf16be(bytes text)
            {
                std::string out;
                for (std::size_t i = 0; i + 1 < text.size(); i += 2)
                {
                    char32_t unit = text.u16(i);
                    if (unit >= 0xD800u && unit <= 0xDBFFu && i + 3 < text.size())
                    {
                        const char32_t low = text.u16(i + 2);
                        if (low >= 0xDC00u && low <= 0xDFFFu)
                        {
                            unit = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
                            i += 2;
                        }
                    }
                    append_utf8(out, unit); // a lone surrogate becomes U+FFFD
                }
                return out;
            }

            // Mac Roman's upper half differs from Latin-1, but the names that
            // only exist in Mac encoding are old and nearly always ASCII.
            std::string decode_mac_roman(bytes text)
            {
                std::string out;
                for (std::size_t i = 0; i < text.size(); ++i)
                    append_utf8(out, text.u8(i) < 0x80u ? text.u8(i) : replacement_character);
                return out;
            }

            // The best record for `name_id`: Windows Unicode in US English,
            // then any Windows or Unicode-platform one, then Mac Roman.
            std::string find_name(bytes table, std::uint16_t name_id)
            {
                const std::size_t count = table.fit(6, table.u16(2), 12);
                const std::size_t storage = table.u16(4);
                int best_score = 0;
                bytes best;
                bool best_utf16 = true;
                for (std::size_t i = 0; i < count; ++i)
                {
                    const std::size_t record = 6 + i * 12;
                    if (table.u16(record + 6) != name_id)
                        continue;
                    const std::uint16_t platform = table.u16(record);
                    const std::uint16_t encoding = table.u16(record + 2);
                    const std::uint16_t language = table.u16(record + 4);

                    int score = 0;
                    bool utf16 = true;
                    if (platform == 3 && (encoding == 1 || encoding == 10))
                        score = language == 0x409 ? 4 : 3;
                    else if (platform == 0)
                        score = 2;
                    else if (platform == 1 && encoding == 0)
                    {
                        score = 1;
                        utf16 = false;
                    }
                    if (score <= best_score)
                        continue;

                    const bytes text = table.sub(storage + table.u16(record + 10), table.u16(record + 8));
                    if (text.empty())
                        continue;
                    best_score = score;
                    best = text;
                    best_utf16 = utf16;
                }
                if (best_score == 0)
                    return {};
                return best_utf16 ? decode_utf16be(best) : decode_mac_roman(best);
            }

            // ---- metrics --------------------------------------------------

            int negative(int value) noexcept
            {
                return value > 0 ? -value : value;
            }

            void parse_metrics(face &face)
            {
                font_metrics &m = face.metrics;
                m.units_per_em = face.head.u16(18);
                m.bounds.add({static_cast<float>(face.head.i16(36)), static_cast<float>(face.head.i16(38))});
                m.bounds.add({static_cast<float>(face.head.i16(40)), static_cast<float>(face.head.i16(42))});

                // Some fonts store the descent as a positive distance; every
                // descent here is negative.
                m.hhea = {face.hhea.i16(4), negative(face.hhea.i16(6)), face.hhea.i16(8)};

                bool use_typo = false;
                const bytes os2 = face.os2;
                if (os2.size() >= 78)
                {
                    m.typo = {os2.i16(68), negative(os2.i16(70)), os2.i16(72)};
                    m.win = {os2.u16(74), -static_cast<int>(os2.u16(76)), 0};
                    use_typo = (os2.u16(62) & 0x80u) != 0 && (m.typo.ascent != 0 || m.typo.descent != 0);
                }

                if (use_typo)
                    m.line = m.typo;
                else if (m.hhea.ascent != 0 || m.hhea.descent != 0)
                    m.line = m.hhea;
                else if (m.typo.ascent != 0 || m.typo.descent != 0)
                    m.line = m.typo;
                else if (m.win.ascent != 0 || m.win.descent != 0)
                    m.line = m.win;
                else if (!m.bounds.empty())
                    m.line = {static_cast<int>(m.bounds.max[1]), static_cast<int>(m.bounds.min[1]), 0};

                if (os2.size() >= 90 && os2.u16(0) >= 2)
                {
                    m.x_height = os2.i16(86);
                    m.cap_height = os2.i16(88);
                }

                const int em = m.units_per_em;
                if (face.post.size() >= 12 && face.post.i16(10) > 0)
                    m.underline = {face.post.i16(8), face.post.i16(10)};
                else
                    m.underline = {-em / 10, std::max(1, em / 20)};

                if (os2.size() >= 30 && os2.i16(26) > 0)
                    m.strikeout = {os2.i16(28), os2.i16(26)};
                else
                    m.strikeout = {0, m.underline.thickness}; // position set once x_height is known
            }

            // Fills in what OS/2 did not say, from the outlines. Needs the
            // cmap and the glyph data, so it runs last.
            void derive_metrics(face &face)
            {
                font_metrics &m = face.metrics;
                const auto top_of = [&](char32_t code_point) {
                    const glyph_id glyph = map_code_point(face, code_point);
                    if (glyph == 0)
                        return 0;
                    const auto shape = face.format == outline_format::cff ? cff_outline(face, glyph)
                                                                           : glyf_outline(face, glyph);
                    if (!shape || shape->empty())
                        return 0;
                    return static_cast<int>(std::lround(shape->control_bounds().max[1]));
                };
                if (m.x_height == 0)
                    m.x_height = top_of(U'x');
                if (m.cap_height == 0)
                    m.cap_height = top_of(U'H');
                if (m.strikeout.position == 0)
                    m.strikeout.position = m.x_height != 0 ? m.x_height / 2 + m.strikeout.thickness / 2
                                                           : m.units_per_em / 4;
            }

            std::expected<std::shared_ptr<face>, std::string>
            parse(std::shared_ptr<const std::vector<std::uint8_t>> storage, std::uint32_t index)
            {
                auto result = std::make_shared<face>();
                face &f = *result;
                f.storage = std::move(storage);
                f.file = bytes(*f.storage);
                f.index = index;

                if (f.file.size() < 12)
                    return std::unexpected("too short to be a font file");

                std::size_t offset = 0;
                if (f.file.u32(0) == collection_tag)
                {
                    if (index >= collection_count(f.file))
                        return std::unexpected("the collection has no face " + std::to_string(index));
                    offset = f.file.u32(12 + std::size_t{index} * 4);
                }
                else if (index != 0)
                {
                    return std::unexpected("not a collection, so there is no face " + std::to_string(index));
                }

                const std::uint32_t version = f.file.u32(offset);
                if (version == tag('O', 'T', 'T', 'O'))
                    f.format = outline_format::cff;
                else if (version == 0x00010000u || version == tag('t', 'r', 'u', 'e'))
                    f.format = outline_format::truetype;
                else if (version == tag('w', 'O', 'F', 'F') || version == tag('w', 'O', 'F', '2'))
                    return std::unexpected("WOFF fonts are compressed and not supported; use the TTF or OTF");
                else
                    return std::unexpected("not a TrueType or OpenType font");

                // The table directory.
                const std::size_t table_count = f.file.fit(offset + 12, f.file.u16(offset + 4), 16);
                if (table_count == 0)
                    return std::unexpected("the font has no tables");
                for (std::size_t i = 0; i < table_count; ++i)
                {
                    const std::size_t record = offset + 12 + i * 16;
                    // Tables running past the end are cut short rather than
                    // refused: the last table is often missing its padding.
                    const bytes table = f.file.sub(f.file.u32(record + 8), f.file.u32(record + 12));
                    switch (f.file.u32(record))
                    {
                    case tag('h', 'e', 'a', 'd'): f.head = table; break;
                    case tag('h', 'h', 'e', 'a'): f.hhea = table; break;
                    case tag('m', 'a', 'x', 'p'): f.maxp = table; break;
                    case tag('h', 'm', 't', 'x'): f.hmtx = table; break;
                    case tag('c', 'm', 'a', 'p'): f.cmap = table; break;
                    case tag('l', 'o', 'c', 'a'): f.loca = table; break;
                    case tag('g', 'l', 'y', 'f'): f.glyf = table; break;
                    case tag('k', 'e', 'r', 'n'): f.kern = table; break;
                    case tag('O', 'S', '/', '2'): f.os2 = table; break;
                    case tag('n', 'a', 'm', 'e'): f.name = table; break;
                    case tag('p', 'o', 's', 't'): f.post = table; break;
                    case tag('C', 'F', 'F', ' '): f.cff = table; break;
                    case tag('G', 'P', 'O', 'S'): f.gpos = table; break;
                    default: break;
                    }
                }

                if (f.head.size() < 54)
                    return std::unexpected("the 'head' table is missing or short");
                if (f.hhea.size() < 36)
                    return std::unexpected("the 'hhea' table is missing or short");
                if (f.maxp.size() < 6)
                    return std::unexpected("the 'maxp' table is missing or short");
                if (f.cmap.empty())
                    return std::unexpected("the font has no 'cmap' table");

                const int units_per_em = f.head.u16(18);
                if (units_per_em < 16 || units_per_em > 16384)
                    return std::unexpected("unitsPerEm is " + std::to_string(units_per_em) +
                                           ", outside the valid 16 to 16384");

                f.glyph_count = f.maxp.u16(4);
                if (f.glyph_count == 0)
                    return std::unexpected("the font has no glyphs");

                f.long_metrics = std::min<std::uint32_t>(f.hhea.u16(34), f.glyph_count);
                if (f.long_metrics == 0)
                    return std::unexpected("the 'hhea' table lists no horizontal metrics");

                if (f.format == outline_format::truetype)
                {
                    if (f.loca.empty() || f.glyf.empty())
                        return std::unexpected("a TrueType font without 'loca' and 'glyf'");
                    f.long_loca = f.head.i16(50) != 0;
                }
                else
                {
                    if (f.cff.empty())
                        return std::unexpected("an OpenType font without a 'CFF ' table (CFF2 is not supported)");
                    if (auto parsed = parse_cff(f); !parsed)
                        return std::unexpected(parsed.error());
                }

                parse_metrics(f);
                f.family = find_name(f.name, 16);
                if (f.family.empty())
                    f.family = find_name(f.name, 1);
                f.style = find_name(f.name, 17);
                if (f.style.empty())
                    f.style = find_name(f.name, 2);

                parse_cmap(f);
                parse_kerning(f);
                derive_metrics(f);

                f.id = next_id.fetch_add(1, std::memory_order_relaxed);
                return result;
            }

            std::shared_ptr<const std::vector<std::uint8_t>> copy(std::span<const std::byte> bytes)
            {
                const auto *begin = reinterpret_cast<const std::uint8_t *>(bytes.data());
                return std::make_shared<const std::vector<std::uint8_t>>(begin, begin + bytes.size());
            }
        } // namespace

        bytes cff_index::item(std::uint32_t index) const noexcept
        {
            if (index >= count)
                return {};
            const std::uint32_t first = data.uint(offsets + std::size_t{index} * offset_size, offset_size);
            const std::uint32_t last = data.uint(offsets + (std::size_t{index} + 1) * offset_size, offset_size);
            if (first == 0 || last < first)
                return {};
            return data.sub(base + first, last - first);
        }
    } // namespace detail

    // ---------------------------------------------------------------------
    // font
    // ---------------------------------------------------------------------

    std::expected<font, std::string> font::load(std::span<const std::byte> bytes, std::uint32_t index)
    {
        auto face = detail::parse(detail::copy(bytes), index);
        if (!face)
            return std::unexpected(face.error());
        return font(std::move(*face));
    }

    std::expected<font, std::string> font::load_file(const std::filesystem::path &path, std::uint32_t index)
    {
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error)
            return std::unexpected("cannot read " + path.string() + ": " + error.message());
        if (size > detail::max_file_size)
            return std::unexpected(path.string() + " is too large to be a font");

        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return std::unexpected("cannot open " + path.string());
        std::vector<std::byte> contents(static_cast<std::size_t>(size));
        stream.read(reinterpret_cast<char *>(contents.data()), static_cast<std::streamsize>(contents.size()));
        if (stream.gcount() != static_cast<std::streamsize>(contents.size()))
            return std::unexpected("cannot read " + path.string());

        auto loaded = load(contents, index);
        if (!loaded)
            return std::unexpected(path.string() + ": " + loaded.error());
        return loaded;
    }

    std::expected<std::vector<font>, std::string> font::load_collection(std::span<const std::byte> bytes)
    {
        const auto count = face_count(bytes);
        if (!count)
            return std::unexpected(count.error());

        const auto storage = detail::copy(bytes);
        std::vector<font> fonts;
        for (std::uint32_t index = 0; index < *count; ++index)
        {
            auto face = detail::parse(storage, index);
            if (!face)
                return std::unexpected("face " + std::to_string(index) + ": " + face.error());
            fonts.push_back(font(std::move(*face)));
        }
        return fonts;
    }

    std::expected<std::uint32_t, std::string> font::face_count(std::span<const std::byte> bytes)
    {
        const detail::bytes file(
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
        if (file.size() < 12)
            return std::unexpected("too short to be a font file");
        const std::uint32_t count = detail::collection_count(file);
        if (count == 0)
            return std::unexpected("the collection holds no faces");
        return count;
    }

    std::uint64_t font::id() const noexcept
    {
        return m_face ? m_face->id : 0;
    }

    std::uint32_t font::index() const noexcept
    {
        return m_face ? m_face->index : 0;
    }

    const std::string &font::family() const noexcept
    {
        static const std::string none;
        return m_face ? m_face->family : none;
    }

    const std::string &font::style() const noexcept
    {
        static const std::string none;
        return m_face ? m_face->style : none;
    }

    outline_format font::format() const noexcept
    {
        return m_face ? m_face->format : outline_format::truetype;
    }

    int font::units_per_em() const noexcept
    {
        return m_face ? m_face->metrics.units_per_em : 0;
    }

    std::uint32_t font::glyph_count() const noexcept
    {
        return m_face ? m_face->glyph_count : 0;
    }

    const font_metrics &font::metrics() const noexcept
    {
        static const font_metrics none;
        return m_face ? m_face->metrics : none;
    }

    float font::scale(float size) const noexcept
    {
        return m_face ? size / static_cast<float>(m_face->metrics.units_per_em) : 0.0f;
    }

    glyph_id font::glyph(char32_t code_point) const noexcept
    {
        return m_face ? detail::map_code_point(*m_face, code_point) : glyph_id{0};
    }

    glyph_metrics font::metrics(glyph_id glyph) const noexcept
    {
        glyph_metrics result;
        if (!m_face || glyph >= m_face->glyph_count)
            return result;

        const detail::face &f = *m_face;
        // Glyphs past the long metrics share the last advance and have only
        // a bearing of their own, in the array that follows.
        if (glyph < f.long_metrics)
        {
            result.advance = f.hmtx.u16(std::size_t{glyph} * 4);
            result.left_side_bearing = f.hmtx.i16(std::size_t{glyph} * 4 + 2);
        }
        else
        {
            result.advance = f.hmtx.u16((std::size_t{f.long_metrics} - 1) * 4);
            result.left_side_bearing =
                f.hmtx.i16(std::size_t{f.long_metrics} * 4 + (std::size_t{glyph} - f.long_metrics) * 2);
        }

        if (f.format == outline_format::truetype)
        {
            result.bounds = detail::glyf_bounds(f, glyph);
        }
        else if (const auto shape = detail::cff_outline(f, glyph))
        {
            result.bounds = shape->bounds();
        }
        return result;
    }

    int font::advance(glyph_id glyph) const noexcept
    {
        if (!m_face || glyph >= m_face->glyph_count)
            return 0;
        const std::size_t entry = std::min<std::size_t>(glyph, m_face->long_metrics - 1);
        return m_face->hmtx.u16(entry * 4);
    }

    int font::kerning(glyph_id left, glyph_id right) const noexcept
    {
        return m_face ? detail::kerning(*m_face, left, right) : 0;
    }

    bool font::has_kerning() const noexcept
    {
        return m_face && (m_face->kern_count != 0 || !m_face->gpos_pair_lookups.empty());
    }

    std::expected<outline, std::string> font::glyph_outline(glyph_id glyph) const
    {
        if (!m_face)
            return std::unexpected("the font is empty");
        if (glyph >= m_face->glyph_count)
            return std::unexpected("glyph " + std::to_string(glyph) + " is out of range");
        if (m_face->format == outline_format::cff)
            return detail::cff_outline(*m_face, glyph);
        return detail::glyf_outline(*m_face, glyph);
    }

} // namespace volt::font
