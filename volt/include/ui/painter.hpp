#pragma once

#include <ui/draw_list.hpp>
#include <ui/geometry.hpp>
#include <ui/text.hpp>
#include <ui/theme.hpp>

#include <font/text.hpp>
#include <math/vector.hpp>

#include <span>
#include <string_view>

// -------------------------------------------------------------------------
// painter
// -------------------------------------------------------------------------
//
// What a widget draws with: the draw list, seen in logical units. Widgets
// are laid out in logical units, and the painter turns those into pixels by
// the interface scale (1 on a 96 dpi display, 2 on one with twice the
// density): logical units in, pixels out.
//
// Rectangles are snapped: each edge goes to the nearest whole pixel, so that
// fills and 1 unit borders are crisp at any scale, and two rectangles that
// share an edge in logical units share it in pixels too. Lines and points
// are scaled but not snapped. Border widths are at least one pixel.
//
// Text is laid out in logical units and drawn at the scaled size, by the
// text engine, so it is rasterized for the pixels it lands on.

namespace volt::ui
{

    // Built-in vector icons, drawn with lines and shapes. They stand in
    // until icons come from image files; see icon() below.
    enum class icon : std::uint8_t
    {
        none,
        check,
        chevron_right,
        chevron_down,
        chevron_up,
        chevron_left,
        close,
        plus,
        minus,
        play,
        stop,
        zoom_in,
        zoom_out,
        fit,
        grid,
        folder,
        file,
        component,
        net,
        wire,
        undo,
        redo,
        save,
        open,
        settings,
        cube,
        sheet,

        // The schematic editor's tools.
        pointer, // select
        tag,     // net label
        power,   // power port
        text,

        // The board editor's tools and switches.
        eye,     // shown
        eye_off, // hidden
        route,   // a track between two pads
        via,
        zone,
        ruler,   // measure
        layers,
        target,  // frame the selection
    };

    class painter
    {
    public:
        painter(draw_list &list, text_engine &text, const ui::theme &theme, float scale) noexcept
            : m_list(list), m_text(text), m_theme(theme), m_scale(scale)
        {
        }

        draw_list &list() noexcept { return m_list; }
        text_engine &text() noexcept { return m_text; }
        const ui::theme &theme() const noexcept { return m_theme; }
        float scale() const noexcept { return m_scale; }

        // ---- units -----------------------------------------------------------

        // A logical rectangle in pixels, its edges on whole pixels.
        rect snap(const rect &area) const noexcept;
        math::vec2f to_pixels(const math::vec2f &point) const noexcept { return point * m_scale; }

        // A logical length in whole pixels, at least `minimum`.
        float pixels(float length, float minimum = 1.0f) const noexcept;

        // ---- shapes -----------------------------------------------------------

        void fill(const rect &area, const math::vec4f &color, float radius = 0.0f);

        // Filled, with a border `width` units wide inside the edge.
        void frame(const rect &area, const math::vec4f &fill, const math::vec4f &border, float radius = 0.0f,
                   float width = 1.0f);

        // Just the border.
        void outline(const rect &area, const math::vec4f &border, float radius = 0.0f, float width = 1.0f);

        void shadow(const rect &area, float radius, float blur, const math::vec4f &color);
        void line(const math::vec2f &from, const math::vec2f &to, float width, const math::vec4f &color);
        void polyline(std::span<const math::vec2f> points, float width, const math::vec4f &color);
        void image(const rect &area, texture_id texture, const math::vec4f &tint = {1.0f, 1.0f, 1.0f, 1.0f},
                   float radius = 0.0f);

        // A built-in icon, centred in `area`.
        void draw_icon(ui::icon which, const rect &area, const math::vec4f &color);

        // The keyboard focus indicator around `area`.
        void focus_ring(const rect &area, float radius);

        // ---- text -------------------------------------------------------------

        // One line in `area`: vertically centred, aligned horizontally as
        // asked, cut with an ellipsis if it does not fit.
        void text(const rect &area, std::string_view text, const text_style &style, const math::vec4f &color,
                  align horizontal = align::start);

        // A run with its pen starting at `baseline` (logical).
        void run(const font::shaped_run &run, const text_style &style, const math::vec2f &baseline,
                 const math::vec4f &color);

        // A layout with its top-left corner at `origin` (logical).
        void layout(const font::text_layout &layout, const text_style &style, const math::vec2f &origin,
                    const math::vec4f &color);

        // Where the baseline goes for one line of `style` centred in a box
        // from `top`, `height` high, in logical units (snapped to pixels).
        float baseline(float top, float height, const text_style &style);

        // ---- clipping ----------------------------------------------------------

        void push_clip(const rect &area);
        void pop_clip();

        // The clip, back in logical units: what can be seen.
        rect visible() const noexcept;

    private:
        draw_list &m_list;
        text_engine &m_text;
        const ui::theme &m_theme;
        float m_scale;
    };

    // Pops on the way out of a scope.
    class clip_scope
    {
    public:
        clip_scope(painter &p, const rect &area) : m_painter(p) { p.push_clip(area); }
        ~clip_scope() { m_painter.pop_clip(); }
        clip_scope(const clip_scope &) = delete;
        clip_scope &operator=(const clip_scope &) = delete;

    private:
        painter &m_painter;
    };

} // namespace volt::ui
