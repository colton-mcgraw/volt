#include <ui/text.hpp>

#include <cmath>

namespace volt::ui
{

    font::atlas_options font_text_engine::default_atlas()
    {
        font::atlas_options options;
        options.page_size = 1024;
        options.padding = 1;
        options.subpixel_steps = 4;
        options.max_pages = 4;
        options.content = font::atlas_content::coverage;
        return options;
    }

    font_text_engine::font_text_engine(const font::atlas_options &options) : m_atlas(options)
    {
    }

    font_id font_text_engine::add(font::font_chain fonts)
    {
        m_fonts.push_back(std::move(fonts));
        return static_cast<font_id>(m_fonts.size() - 1);
    }

    const font::font_chain &font_text_engine::chain(font_id id) const noexcept
    {
        if (id < m_fonts.size())
            return m_fonts[id];
        return m_fonts.empty() ? m_empty : m_fonts.front();
    }

    void font_text_engine::reset()
    {
        m_atlas.clear();
        m_overflowed = false;
    }

    text_metrics font_text_engine::metrics(const text_style &style)
    {
        const font::font &primary = chain(style.font).primary();
        const float scale = primary.scale(style.size);
        const font::line_metrics &line = primary.metrics().line;
        return {static_cast<float>(line.ascent) * scale, static_cast<float>(line.descent) * scale,
                static_cast<float>(line.height()) * scale};
    }

    font::shaped_run font_text_engine::shape(std::string_view text, const text_style &style)
    {
        return font::shape(chain(style.font), text, style.size);
    }

    float font_text_engine::measure(std::string_view text, const text_style &style)
    {
        return font::measure(chain(style.font), text, style.size);
    }

    font::text_layout font_text_engine::layout(std::string_view text, const text_style &style,
                                               const font::layout_options &options)
    {
        font::layout_options adjusted = options;
        adjusted.size = style.size;
        return font::layout(chain(style.font), text, adjusted);
    }

    float font_text_engine::contrast_for(const math::vec4f &color) const noexcept
    {
        // Relative luminance of the (linear) colour picks between the two.
        const float luminance = 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2];
        const float t = std::clamp(luminance, 0.0f, 1.0f);
        return m_contrast_dark + (m_contrast_light - m_contrast_dark) * t;
    }

    void font_text_engine::draw_glyph(draw_list &list, const font::font_chain &fonts, std::uint16_t font,
                                      font::glyph_id glyph, float pixel_size, const math::vec2f &pen,
                                      const math::vec4f &color, float contrast)
    {
        if (font >= fonts.fonts().size())
            return;

        const auto quad = m_atlas.quad(fonts.fonts()[font], glyph, pixel_size, pen);
        if (!quad)
        {
            m_overflowed = true;
            return;
        }
        if (quad->empty())
            return;

        list.glyph(rect::from(quad->position, quad->size), texture_id::glyph_page(quad->page), quad->uv_min,
                   quad->uv_max, color, contrast);
    }

    void font_text_engine::draw(draw_list &list, const font::text_layout &layout, const text_style &style,
                                const math::vec2f &origin, float scale, const math::vec4f &color)
    {
        const font::font_chain &fonts = chain(style.font);
        const float pixel_size = style.size * scale;
        const float contrast = contrast_for(color);

        for (const font::text_line &line : layout.lines)
        {
            // Every glyph of a line shares its baseline, snapped once.
            const float baseline = std::round(origin.y() + line.baseline * scale);
            for (std::uint32_t i = 0; i < line.glyph_count; ++i)
            {
                const font::positioned_glyph &g = layout.glyphs[line.first_glyph + i];
                const math::vec2f pen{origin.x() + g.position.x() * scale, baseline};
                draw_glyph(list, fonts, g.font, g.glyph, pixel_size, pen, color, contrast);
            }
        }
    }

    void font_text_engine::draw(draw_list &list, const font::shaped_run &run, const text_style &style,
                                const math::vec2f &baseline, float scale, const math::vec4f &color)
    {
        const font::font_chain &fonts = chain(style.font);
        const float pixel_size = style.size * scale;
        const float contrast = contrast_for(color);
        const float y = std::round(baseline.y());

        const rect &clip = list.current_clip();
        for (const font::shaped_glyph &g : run.glyphs)
        {
            const float x = baseline.x() + g.x * scale;

            // A long run in a narrow field: what is outside is not placed in
            // the atlas at all.
            if (x > clip.right() || x + (g.advance + style.size) * scale < clip.x)
                continue;
            draw_glyph(list, fonts, g.font, g.glyph, pixel_size, {x, y}, color, contrast);
        }
    }

} // namespace volt::ui
