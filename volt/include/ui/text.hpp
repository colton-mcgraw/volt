#pragma once

#include <ui/draw_list.hpp>

#include <font/atlas.hpp>
#include <font/text.hpp>
#include <math/vector.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// text
// -------------------------------------------------------------------------
//
// How the interface measures and draws text. Widgets only ever talk to the
// text_engine interface, which the framework owns; font_text_engine is the
// implementation on top of the font library, and the only one there is.
// The interface exists so that what the widgets need from text is written
// down in one place, and so that a test can count calls or stand in sizes
// without changing a widget.
//
// Two units meet here. Measuring and laying out happen in logical units,
// like the rest of the layout, at the style's size: shaping is unhinted and
// linear in the size (<font/text.hpp>), so a layout made at 13 logical units
// is exactly the layout at 13 * scale pixels, scaled. Drawing takes such a
// layout and the interface scale, and rasterizes the glyphs at the scaled
// size in pixels, so text is as crisp at a scale of 2 as at 1. Baselines are
// snapped to whole pixels; horizontally the atlas keeps subpixel positions.
//
// One thread: the one the interface lives on.

namespace volt::ui
{

    // Which of the engine's fonts: the value add() returned.
    using font_id = std::uint16_t;

    struct text_style
    {
        font_id font = 0;
        float size = 13.0f; // em size, logical units

        friend bool operator==(const text_style &, const text_style &) = default;
    };

    // Of the style's primary font, in logical units. Descent is negative
    // (below the baseline), as in <font/font.hpp>.
    struct text_metrics
    {
        float ascent = 0.0f;
        float descent = 0.0f;
        float line_height = 0.0f; // baseline to baseline

        float height() const noexcept { return ascent - descent; }
    };

    class text_engine
    {
    public:
        virtual ~text_engine() = default;

        virtual text_metrics metrics(const text_style &style) = 0;

        // One line, no breaking. Positions in logical units from the pen
        // start (font::shape()).
        virtual font::shaped_run shape(std::string_view text, const text_style &style) = 0;

        // Its width.
        virtual float measure(std::string_view text, const text_style &style) = 0;

        // Lines broken and aligned as `options` say. `options.size` is
        // ignored in favour of the style's.
        virtual font::text_layout layout(std::string_view text, const text_style &style,
                                         const font::layout_options &options) = 0;

        // Adds the glyphs of a layout made at `style`, its top-left corner
        // at `origin` (pixels), drawn at `scale` pixels per logical unit.
        virtual void draw(draw_list &list, const font::text_layout &layout, const text_style &style,
                          const math::vec2f &origin, float scale, const math::vec4f &color) = 0;

        // The same for a run, its pen starting at `baseline` (pixels).
        virtual void draw(draw_list &list, const font::shaped_run &run, const text_style &style,
                          const math::vec2f &baseline, float scale, const math::vec4f &color) = 0;

        // Whether a draw() since the last reset() could not place a glyph in
        // its atlas, and the reset that makes room again (see
        // font_text_engine). An engine without an atlas never overflows.
        virtual bool overflowed() const noexcept { return false; }
        virtual void reset() {}
    };

    // ---------------------------------------------------------------------
    // font_text_engine
    // ---------------------------------------------------------------------
    //
    // The text engine on top of <font/text.hpp>, owning the glyph atlas the
    // glyphs it draws come from. The atlas's pages are what the draw list's
    // glyph page ids refer to; the renderer mirrors them into textures
    // (<app/ui/glyph_cache.hpp>) and uploads what changed each frame.
    //
    // When the atlas is full, a glyph is left out and overflowed() turns
    // true. The frame that ran into it should then be built again from the
    // start after reset(): glyphs placed earlier in it point into pages the
    // reset forgets, so drawing it as it is would show garbage for a frame.
    // <ui/screen.hpp> does exactly that.
    class font_text_engine final : public text_engine
    {
    public:
        explicit font_text_engine(const font::atlas_options &options = default_atlas());

        // A coverage atlas of a few pages of 1024 squared, which holds the
        // glyphs of an interface at several sizes many times over.
        static font::atlas_options default_atlas();

        // Adds fonts to choose from; the first add() is font 0, the default.
        font_id add(font::font_chain fonts);
        std::size_t font_count() const noexcept { return m_fonts.size(); }

        // How much heavier than plain coverage glyphs are drawn, depending
        // on how light the text is: blending coverage on encoded values
        // thins light text on a dark ground, and on linear ones thins dark
        // text on a light ground. 0 is none.
        void set_contrast(float dark_text, float light_text) noexcept
        {
            m_contrast_dark = dark_text;
            m_contrast_light = light_text;
        }

        font::atlas &atlas() noexcept { return m_atlas; }
        const font::atlas &atlas() const noexcept { return m_atlas; }

        bool overflowed() const noexcept override { return m_overflowed; }

        // Empties the atlas (every glyph is placed again as it is next
        // drawn) and forgets the overflow.
        void reset() override;

        text_metrics metrics(const text_style &style) override;
        font::shaped_run shape(std::string_view text, const text_style &style) override;
        float measure(std::string_view text, const text_style &style) override;
        font::text_layout layout(std::string_view text, const text_style &style,
                                 const font::layout_options &options) override;
        void draw(draw_list &list, const font::text_layout &layout, const text_style &style,
                  const math::vec2f &origin, float scale, const math::vec4f &color) override;
        void draw(draw_list &list, const font::shaped_run &run, const text_style &style,
                  const math::vec2f &baseline, float scale, const math::vec4f &color) override;

    private:
        const font::font_chain &chain(font_id id) const noexcept;
        float contrast_for(const math::vec4f &color) const noexcept;

        // One glyph with its pen at `pen` (pixels), at `pixel_size`.
        void draw_glyph(draw_list &list, const font::font_chain &fonts, std::uint16_t font, font::glyph_id glyph,
                        float pixel_size, const math::vec2f &pen, const math::vec4f &color, float contrast);

        std::vector<font::font_chain> m_fonts;
        font::font_chain m_empty;
        font::atlas m_atlas;
        bool m_overflowed = false;
        float m_contrast_dark = 0.0f;
        float m_contrast_light = 0.0f;
    };

} // namespace volt::ui
