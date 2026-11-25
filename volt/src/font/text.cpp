#include <font/text.hpp>

#include <font/utf8.hpp>

#include <algorithm>
#include <cmath>

namespace volt::font
{

    namespace
    {
        // ---- character classes -------------------------------------------
        //
        // A small subset of what Unicode's line breaking algorithm (UAX #14)
        // says, enough for the scripts this file shapes.

        bool is_line_break(char32_t c) noexcept
        {
            return c == U'\n' || c == U'\v' || c == U'\f' || c == U'\r' || c == 0x85u || c == 0x2028u ||
                   c == 0x2029u;
        }

        // Breaking spaces. Not U+00A0, U+2007 or U+202F, which exist so that
        // lines do not break at them.
        bool is_space(char32_t c) noexcept
        {
            return c == U' ' || c == U'\t' || c == 0x1680u || (c >= 0x2000u && c <= 0x2006u) ||
                   (c >= 0x2008u && c <= 0x200Au) || c == 0x205Fu || c == 0x3000u;
        }

        // Invisible and without width: controls, and what Unicode calls
        // default ignorable (soft hyphen, zero width space and joiners,
        // direction marks, variation selectors, the BOM).
        bool is_invisible(char32_t c) noexcept
        {
            return c < 0x20u || (c >= 0x7Fu && c <= 0x9Fu) || c == 0xADu || (c >= 0x200Bu && c <= 0x200Fu) ||
                   (c >= 0x202Au && c <= 0x202Eu) || (c >= 0x2060u && c <= 0x2064u) ||
                   (c >= 0xFE00u && c <= 0xFE0Fu) || c == 0xFEFFu || (c >= 0xE0000u && c <= 0xE0FFFu);
        }

        bool is_ideographic(char32_t c) noexcept
        {
            return (c >= 0x2E80u && c <= 0x2FFFu) || (c >= 0x3040u && c <= 0x31FFu) ||
                   (c >= 0x3400u && c <= 0x4DBFu) || (c >= 0x4E00u && c <= 0x9FFFu) ||
                   (c >= 0xAC00u && c <= 0xD7AFu) || (c >= 0xF900u && c <= 0xFAFFu) ||
                   (c >= 0xFF01u && c <= 0xFF60u) || (c >= 0x20000u && c <= 0x3FFFFu);
        }

        bool break_after(char32_t c) noexcept
        {
            return is_space(c) || c == U'-' || c == 0x2010u || c == 0x2013u || c == 0x2014u || c == 0x200Bu ||
                   is_ideographic(c);
        }

        bool break_before(char32_t c) noexcept
        {
            return is_ideographic(c);
        }

        // ---- shaping -------------------------------------------------------

        struct shaped_text
        {
            std::vector<shaped_glyph> glyphs;
            std::vector<char32_t> code_points;
            float width = 0.0f;
        };

        void shape_text(const font_chain &fonts, std::string_view text, float size, shaped_text &out)
        {
            out.glyphs.clear();
            out.code_points.clear();
            out.width = 0.0f;

            const std::span<const font> list = fonts.fonts();
            const font_chain::resolved space = fonts.resolve(U' ');
            const float space_advance =
                list.empty() ? 0.0f
                             : static_cast<float>(list[space.font].advance(space.glyph)) * list[space.font].scale(size);

            // Kerning applies between neighbours that are both ordinary
            // glyphs of the same font.
            std::vector<bool> kernable;
            for (std::size_t offset = 0; offset < text.size();)
            {
                const decoded d = decode_utf8(text, offset);
                const char32_t c = d.code_point;
                shaped_glyph glyph;
                glyph.cluster = static_cast<std::uint32_t>(offset);
                offset += d.length;

                bool ordinary = false;
                if (c == U'\t')
                {
                    glyph.font = space.font;
                    glyph.glyph = space.glyph;
                    glyph.advance = 4.0f * space_advance;
                }
                else if (is_invisible(c) || is_line_break(c))
                {
                    glyph.font = space.font;
                    glyph.glyph = space.glyph;
                }
                else
                {
                    const font_chain::resolved r = fonts.resolve(c);
                    glyph.font = r.font;
                    glyph.glyph = r.glyph;
                    if (!list.empty())
                    {
                        glyph.advance = static_cast<float>(list[r.font].advance(r.glyph)) * list[r.font].scale(size);
                        ordinary = true;
                    }
                }
                out.glyphs.push_back(glyph);
                out.code_points.push_back(c);
                kernable.push_back(ordinary);
            }

            float x = 0.0f;
            for (std::size_t i = 0; i < out.glyphs.size(); ++i)
            {
                shaped_glyph &glyph = out.glyphs[i];
                if (i + 1 < out.glyphs.size() && kernable[i] && kernable[i + 1] &&
                    glyph.font == out.glyphs[i + 1].font)
                {
                    const font &f = list[glyph.font];
                    glyph.advance += static_cast<float>(f.kerning(glyph.glyph, out.glyphs[i + 1].glyph)) * f.scale(size);
                }
                glyph.x = x;
                x += glyph.advance;
            }
            out.width = x;
        }

        // ---- caret helpers shared by runs and layouts ---------------------

        // The index of the glyph whose code point contains `offset`, or
        // `count` if it is at or past the end.
        template <typename Glyph>
        std::size_t glyph_at(std::span<const Glyph> glyphs, std::size_t offset) noexcept
        {
            // Clusters increase; the last glyph starting at or before.
            const auto after = std::upper_bound(glyphs.begin(), glyphs.end(), offset,
                                                [](std::size_t value, const Glyph &g) { return value < g.cluster; });
            if (after == glyphs.begin())
                return 0;
            return static_cast<std::size_t>(after - glyphs.begin()) - 1;
        }
    } // namespace

    // ---------------------------------------------------------------------
    // font_chain
    // ---------------------------------------------------------------------

    const font &font_chain::primary() const noexcept
    {
        static const font none;
        return m_fonts.empty() ? none : m_fonts.front();
    }

    font_chain::resolved font_chain::resolve(char32_t code_point) const noexcept
    {
        for (std::size_t i = 0; i < m_fonts.size() && i <= 0xFFFFu; ++i)
        {
            const glyph_id glyph = m_fonts[i].glyph(code_point);
            if (glyph != 0)
                return {static_cast<std::uint16_t>(i), glyph};
        }
        return {0, 0};
    }

    // ---------------------------------------------------------------------
    // One line
    // ---------------------------------------------------------------------

    shaped_run shape(const font_chain &fonts, std::string_view text, float size)
    {
        shaped_text shaped;
        shape_text(fonts, text, size, shaped);
        shaped_run run;
        run.glyphs = std::move(shaped.glyphs);
        run.width = shaped.width;
        run.length = text.size();
        return run;
    }

    float measure(const font_chain &fonts, std::string_view text, float size)
    {
        return shape(fonts, text, size).width;
    }

    std::vector<caret_stop> caret_stops(const shaped_run &run)
    {
        std::vector<caret_stop> stops;
        stops.reserve(run.glyphs.size() + 1);
        for (const shaped_glyph &glyph : run.glyphs)
            stops.push_back({glyph.cluster, glyph.x});
        stops.push_back({run.length, run.width});
        return stops;
    }

    float caret_x(const shaped_run &run, std::size_t offset) noexcept
    {
        if (run.glyphs.empty() || offset >= run.length)
            return run.width;
        const std::span<const shaped_glyph> glyphs = run.glyphs;
        return glyphs[glyph_at(glyphs, offset)].x;
    }

    std::size_t hit_test(const shaped_run &run, float x) noexcept
    {
        // Before the middle of a glyph puts the caret before it.
        for (const shaped_glyph &glyph : run.glyphs)
        {
            if (x < glyph.x + glyph.advance * 0.5f)
                return glyph.cluster;
        }
        return run.length;
    }

    // ---------------------------------------------------------------------
    // Paragraphs
    // ---------------------------------------------------------------------

    namespace
    {
        // A line before positioning: glyphs [first, last) of the shaped text.
        struct span_of_line
        {
            std::size_t first = 0;
            std::size_t last = 0;
            std::size_t begin = 0; // bytes
            std::size_t end = 0;
            bool hard_break = false;
            bool truncated = false;
        };

        // Width of glyphs [first, last), without the spaces at the end.
        float visible_width(const shaped_text &text, std::size_t first, std::size_t last) noexcept
        {
            while (last > first && (is_space(text.code_points[last - 1]) || text.glyphs[last - 1].advance == 0.0f))
                --last;
            if (last == first)
                return 0.0f;
            const shaped_glyph &end = text.glyphs[last - 1];
            return end.x + end.advance - text.glyphs[first].x;
        }

        // Splits [first, last) into lines no wider than `width`.
        void wrap(const shaped_text &text, std::size_t first, std::size_t last, float width,
                  std::vector<span_of_line> &lines)
        {
            std::size_t start = first;
            while (start < last)
            {
                const float origin = text.glyphs[start].x;
                std::size_t opportunity = start; // a break before this glyph, if > start
                std::size_t end = last;
                for (std::size_t i = start; i < last; ++i)
                {
                    const char32_t c = text.code_points[i];
                    if (i > start && break_before(c))
                        opportunity = i;
                    const shaped_glyph &glyph = text.glyphs[i];
                    // Spaces may hang past the edge; anything else may not.
                    if (i > start && !is_space(c) && glyph.x + glyph.advance - origin > width)
                    {
                        end = opportunity > start ? opportunity : i;
                        break;
                    }
                    if (break_after(c))
                        opportunity = i + 1;
                }
                lines.push_back({start, end, 0, 0, false, false});
                start = end;
            }
        }

        std::size_t glyph_end(const shaped_text &text, std::size_t index, std::size_t length) noexcept
        {
            return index + 1 < text.glyphs.size() ? text.glyphs[index + 1].cluster : length;
        }
    } // namespace

    text_layout layout(const font_chain &fonts, std::string_view text, const layout_options &options)
    {
        text_layout result;
        result.size = options.size;
        result.length = text.size();

        const font &primary = fonts.primary();
        const float scale = primary.scale(options.size);
        const line_metrics metrics = primary.metrics().line;
        result.ascent = static_cast<float>(metrics.ascent) * scale;
        result.descent = static_cast<float>(metrics.descent) * scale;
        result.line_height = static_cast<float>(metrics.height()) * scale * options.line_spacing;

        shaped_text shaped;
        shape_text(fonts, text, options.size, shaped);
        const std::size_t count = shaped.glyphs.size();
        const bool limited = options.max_width > 0.0f;

        // Paragraphs, then lines within them.
        std::vector<span_of_line> lines;
        std::size_t paragraph = 0;
        std::size_t paragraph_begin = 0;
        for (std::size_t i = 0; i <= count; ++i)
        {
            const bool at_end = i == count;
            bool is_break = !at_end && is_line_break(shaped.code_points[i]);
            // CR LF is one break, at the LF.
            if (is_break && shaped.code_points[i] == U'\r' && i + 1 < count && shaped.code_points[i + 1] == U'\n')
                is_break = false;
            if (!at_end && !is_break)
                continue;

            const std::size_t before = lines.size();
            if (options.wrap && limited)
                wrap(shaped, paragraph, i, options.max_width, lines);
            if (lines.size() == before)
                lines.push_back({paragraph, i, 0, 0, false, false});
            // The CR of a CR LF stays in the line as a blank glyph.
            for (std::size_t l = before; l < lines.size(); ++l)
            {
                span_of_line &line = lines[l];
                line.begin = line.first < count ? shaped.glyphs[line.first].cluster : text.size();
                line.end = line.last < count ? shaped.glyphs[line.last].cluster : text.size();
            }
            lines[before].begin = paragraph_begin;
            lines.back().hard_break = !at_end;

            paragraph = i + 1;
            paragraph_begin = at_end ? text.size() : glyph_end(shaped, i, text.size());
        }

        // Too many lines: the last one kept is cut.
        if (options.max_lines > 0 && lines.size() > options.max_lines)
        {
            lines.resize(options.max_lines);
            lines.back().truncated = true;
            lines.back().hard_break = false;
        }
        // Too wide without wrapping: cut.
        if (!options.wrap && limited)
        {
            for (span_of_line &line : lines)
            {
                if (visible_width(shaped, line.first, line.last) > options.max_width)
                    line.truncated = true;
            }
        }

        // The ellipsis: U+2026 if any font has it, else three periods.
        std::vector<positioned_glyph> ellipsis;
        float ellipsis_width = 0.0f;
        if (options.ellipsis && !fonts.empty())
        {
            const font_chain::resolved single = fonts.resolve(0x2026u);
            const font_chain::resolved period = fonts.resolve(U'.');
            const auto append = [&](const font_chain::resolved &r) {
                const font &f = fonts.fonts()[r.font];
                const float advance = static_cast<float>(f.advance(r.glyph)) * f.scale(options.size);
                ellipsis.push_back({r.font, r.glyph, 0, {ellipsis_width, 0.0f}, advance});
                ellipsis_width += advance;
            };
            if (single.glyph != 0)
                append(single);
            else
                for (int i = 0; i < 3; ++i)
                    append(period);
        }

        // Position the lines.
        result.lines.reserve(lines.size());
        float widest = 0.0f;
        for (std::size_t l = 0; l < lines.size(); ++l)
        {
            span_of_line &span = lines[l];
            text_line line;
            line.begin = static_cast<std::uint32_t>(span.begin);
            line.end = static_cast<std::uint32_t>(span.end);
            line.baseline = result.ascent + static_cast<float>(l) * result.line_height;
            line.hard_break = span.hard_break;
            line.first_glyph = static_cast<std::uint32_t>(result.glyphs.size());

            std::size_t last = span.last;
            if (span.truncated && !ellipsis.empty())
            {
                // Keep as much as fits beside the ellipsis.
                if (limited)
                {
                    while (last > span.first &&
                           visible_width(shaped, span.first, last) + ellipsis_width > options.max_width)
                        --last;
                }
                while (last > span.first && is_space(shaped.code_points[last - 1]))
                    --last;
                line.truncated = true;
            }
            else if (span.truncated)
            {
                line.truncated = true;
            }

            const float origin = span.first < count ? shaped.glyphs[span.first].x : 0.0f;
            for (std::size_t i = span.first; i < last; ++i)
            {
                const shaped_glyph &g = shaped.glyphs[i];
                result.glyphs.push_back({g.font, g.glyph, g.cluster, {g.x - origin, line.baseline}, g.advance});
            }
            line.width = visible_width(shaped, span.first, last);
            if (line.truncated && !ellipsis.empty())
            {
                const auto cut = static_cast<std::uint32_t>(last < count ? shaped.glyphs[last].cluster : text.size());
                for (positioned_glyph g : ellipsis)
                {
                    g.cluster = cut;
                    g.position = {g.position[0] + line.width, line.baseline};
                    result.glyphs.push_back(g);
                }
                line.width += ellipsis_width;
            }
            line.glyph_count = static_cast<std::uint32_t>(result.glyphs.size()) - line.first_glyph;
            widest = std::max(widest, line.width);
            result.lines.push_back(line);
        }

        const float box = limited ? options.max_width : widest;
        for (text_line &line : result.lines)
        {
            if (options.align == text_align::center)
                line.x = (box - line.width) * 0.5f;
            else if (options.align == text_align::right)
                line.x = box - line.width;
            for (std::uint32_t i = 0; i < line.glyph_count; ++i)
                result.glyphs[line.first_glyph + i].position[0] += line.x;
        }

        result.extent = {box, static_cast<float>(result.lines.size()) * result.line_height};
        return result;
    }

    caret caret_at(const text_layout &layout, std::size_t offset) noexcept
    {
        caret result;
        result.height = layout.ascent - layout.descent;
        if (layout.lines.empty())
            return result;

        // The last line starting at or before the offset.
        std::size_t index = 0;
        while (index + 1 < layout.lines.size() && layout.lines[index + 1].begin <= offset)
            ++index;
        const text_line &line = layout.lines[index];
        result.line = index;

        float x = line.x;
        const std::span<const positioned_glyph> glyphs =
            std::span<const positioned_glyph>(layout.glyphs).subspan(line.first_glyph, line.glyph_count);
        if (!glyphs.empty())
        {
            if (offset >= line.end && !line.truncated)
                x = glyphs.back().position[0] + glyphs.back().advance;
            else
                x = glyphs[glyph_at(glyphs, offset)].position[0];
        }
        result.top = {x, line.baseline - layout.ascent};
        return result;
    }

    std::size_t hit_test(const text_layout &layout, const math::vec2f &point) noexcept
    {
        if (layout.lines.empty() || !(layout.line_height > 0.0f))
            return 0;
        const float row = std::floor(point[1] / layout.line_height);
        const std::size_t index =
            row <= 0.0f ? 0 : std::min(static_cast<std::size_t>(row), layout.lines.size() - 1);
        const text_line &line = layout.lines[index];

        const std::span<const positioned_glyph> glyphs =
            std::span<const positioned_glyph>(layout.glyphs).subspan(line.first_glyph, line.glyph_count);
        for (const positioned_glyph &glyph : glyphs)
        {
            if (point[0] < glyph.position[0] + glyph.advance * 0.5f)
                return std::min<std::size_t>(glyph.cluster, line.end);
        }

        // Past the end. A line that wraps at a space ends in that space, and
        // the offset after it is the start of the next line; the caret stays
        // on this one, before the space. (Trailing spaces are the glyphs
        // that start at or past the line's visible width.)
        const bool wrapped = !line.hard_break && index + 1 < layout.lines.size();
        if (wrapped && !line.truncated && !glyphs.empty() &&
            glyphs.back().position[0] >= line.x + line.width - 1e-4f * layout.size)
            return glyphs.back().cluster;
        return line.end;
    }

    outline layout_outline(const font_chain &fonts, const text_layout &layout)
    {
        outline result;
        const std::span<const font> list = fonts.fonts();
        for (const positioned_glyph &glyph : layout.glyphs)
        {
            if (glyph.font >= list.size())
                continue;
            const font &f = list[glyph.font];
            const auto shape = f.glyph_outline(glyph.glyph);
            if (!shape || shape->empty())
                continue;
            const float scale = f.scale(layout.size);
            result.append(*shape, affine::scale(scale, -scale, glyph.position));
        }
        return result;
    }

} // namespace volt::font
