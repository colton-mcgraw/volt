#pragma once

#include <font/atlas.hpp>
#include <font/raster.hpp>
#include <font/text.hpp>

#include <cstddef>
#include <cstdint>

// Everything the font module can be asked about one font, for running on
// fonts that may be damaged: the point is that it returns, not what it
// returns. Shared by the corruption test and the robustness tool.

namespace volt::tests::font
{
    struct exercise_counts
    {
        std::size_t glyphs = 0;
        std::size_t outline_errors = 0;
        std::size_t rasterized = 0;
    };

    // Every glyph's outline and metrics, a rasterization of every
    // `raster_every`-th glyph (0: none), cmap lookups, kerning and a layout.
    inline exercise_counts exercise(const volt::font::font &f, std::uint32_t raster_every)
    {
        using namespace volt::font;
        exercise_counts counts;
        volatile float sink = 0.0f;

        const std::uint32_t count = f.glyph_count();
        for (std::uint32_t id = 0; id < count; ++id)
        {
            const auto glyph = static_cast<glyph_id>(id);
            ++counts.glyphs;
            const glyph_metrics m = f.metrics(glyph);
            sink = sink + static_cast<float>(m.advance) + m.bounds.width();
            const auto shape = f.glyph_outline(glyph);
            if (!shape)
            {
                ++counts.outline_errors;
                continue;
            }
            sink = sink + shape->signed_area() + shape->bounds().width();
            if (raster_every != 0 && id % raster_every == 0)
            {
                if (const auto image = rasterize(*shape, f.scale(24.0f), 0.5f); image)
                {
                    ++counts.rasterized;
                    sink = sink + static_cast<float>(image->pixels.size());
                }
            }
        }

        for (char32_t c = 0; c < 0x3000; c += 7)
            sink = sink + static_cast<float>(f.glyph(c));
        for (char32_t c : {U'\U0001F600', U'\U0010FFFF', char32_t{0x110000}, char32_t{0xFFFFFFFF}})
            sink = sink + static_cast<float>(f.glyph(c));
        for (glyph_id left = 0; left < 64 && left < count; ++left)
            for (glyph_id right = 0; right < 64 && right < count; ++right)
                sink = sink + static_cast<float>(f.kerning(left, right));
        sink = sink + static_cast<float>(f.kerning(65535, 65535));

        const font_chain chain({f});
        layout_options options;
        options.max_width = 200.0f;
        options.max_lines = 3;
        const text_layout laid = layout(chain, "The quick brown fox éÅ AV To … \xFF\xFE end", options);
        sink = sink + laid.extent[0] + static_cast<float>(hit_test(laid, {50.0f, 20.0f}));
        if (raster_every != 0)
        {
            atlas glyphs({.page_size = 256, .max_pages = 2});
            for (const positioned_glyph &g : laid.glyphs)
                (void)glyphs.quad(f, g.glyph, 16.0f, g.position);
            if (count > 0)
                (void)distance_field(f, static_cast<glyph_id>(count / 2), 32.0f, 4.0f);
        }
        (void)sink;
        return counts;
    }
} // namespace volt::tests::font
