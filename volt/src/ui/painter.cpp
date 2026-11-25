#include <ui/painter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace volt::ui
{

    rect painter::snap(const rect &area) const noexcept
    {
        const float left = std::round(area.x * m_scale);
        const float top = std::round(area.y * m_scale);
        const float right = std::round(area.right() * m_scale);
        const float bottom = std::round(area.bottom() * m_scale);
        return {left, top, right - left, bottom - top};
    }

    float painter::pixels(float length, float minimum) const noexcept
    {
        return std::max(minimum, std::round(length * m_scale));
    }

    void painter::fill(const rect &area, const math::vec4f &color, float radius)
    {
        m_list.rectangle(snap(area), color, radius * m_scale);
    }

    void painter::frame(const rect &area, const math::vec4f &fill, const math::vec4f &border, float radius,
                        float width)
    {
        m_list.rectangle(snap(area), fill, radius * m_scale, pixels(width), border);
    }

    void painter::outline(const rect &area, const math::vec4f &border, float radius, float width)
    {
        m_list.rectangle(snap(area), {0.0f, 0.0f, 0.0f, 0.0f}, radius * m_scale, pixels(width), border);
    }

    void painter::shadow(const rect &area, float radius, float blur, const math::vec4f &color)
    {
        m_list.shadow(snap(area), radius * m_scale, blur * m_scale, color);
    }

    void painter::line(const math::vec2f &from, const math::vec2f &to, float width, const math::vec4f &color)
    {
        m_list.line(to_pixels(from), to_pixels(to), std::max(1.0f, width * m_scale), color);
    }

    void painter::polyline(std::span<const math::vec2f> points, float width, const math::vec4f &color)
    {
        for (std::size_t i = 1; i < points.size(); ++i)
            line(points[i - 1], points[i], width, color);
    }

    void painter::image(const rect &area, texture_id texture, const math::vec4f &tint, float radius)
    {
        m_list.image(snap(area), texture, tint, radius * m_scale);
    }

    void painter::focus_ring(const rect &area, float radius)
    {
        const float width = m_theme.focus_width;
        outline(area.outset(width), m_theme.focus_ring, radius + width, width);
    }

    float painter::baseline(float top, float height, const text_style &style)
    {
        const text_metrics metrics = m_text.metrics(style);
        // Centre the ascent-to-descent box, then put the baseline on a pixel.
        const float logical = top + (height - metrics.height()) * 0.5f + metrics.ascent;
        return std::round(logical * m_scale) / m_scale;
    }

    void painter::text(const rect &area, std::string_view text, const text_style &style, const math::vec4f &color,
                       align horizontal)
    {
        if (text.empty() || area.width <= 0.0f)
            return;

        font::layout_options options;
        options.max_width = area.width;
        options.wrap = false;
        options.max_lines = 1;
        options.ellipsis = true;
        options.align = horizontal == align::start    ? font::text_align::left
                        : horizontal == align::center ? font::text_align::center
                                                      : font::text_align::right;

        const font::text_layout laid = m_text.layout(text, style, options);
        if (laid.lines.empty())
            return;

        // Put the line's baseline where baseline() says, whatever the
        // layout's own idea of the top is.
        const float base = baseline(area.y, area.height, style);
        const float top = base - laid.lines.front().baseline;
        layout(laid, style, {area.x, top}, color);
    }

    void painter::run(const font::shaped_run &run, const text_style &style, const math::vec2f &baseline,
                      const math::vec4f &color)
    {
        m_text.draw(m_list, run, style, to_pixels(baseline), m_scale, color);
    }

    void painter::layout(const font::text_layout &layout, const text_style &style, const math::vec2f &origin,
                         const math::vec4f &color)
    {
        m_text.draw(m_list, layout, style, to_pixels(origin), m_scale, color);
    }

    void painter::push_clip(const rect &area)
    {
        m_list.push_clip(snap(area));
    }

    void painter::pop_clip()
    {
        m_list.pop_clip();
    }

    rect painter::visible() const noexcept
    {
        const rect &clip = m_list.current_clip();
        return {clip.x / m_scale, clip.y / m_scale, clip.width / m_scale, clip.height / m_scale};
    }

    // ---------------------------------------------------------------------
    // icons
    // ---------------------------------------------------------------------
    //
    // Drawn on a 16 by 16 grid, centred in the area and scaled to fit it.

    void painter::draw_icon(ui::icon which, const rect &area, const math::vec4f &color)
    {
        using math::vec2f;

        if (which == icon::none)
            return;

        const float size = std::min(area.width, area.height);
        const float unit = size / 16.0f;
        const vec2f origin = area.center() - vec2f{8.0f, 8.0f} * unit;
        const auto at = [&](float x, float y) { return origin + vec2f{x, y} * unit; };
        const float stroke = std::max(1.25f * unit, 1.0f / m_scale);

        const auto path = [&](std::initializer_list<vec2f> grid_points) {
            std::array<vec2f, 16> points{};
            std::size_t count = 0;
            for (const vec2f &p : grid_points)
            {
                if (count < points.size())
                    points[count++] = at(p.x(), p.y());
            }
            polyline(std::span<const vec2f>(points.data(), count), stroke, color);
        };
        const auto box = [&](float x, float y, float w, float h) {
            path({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}, {x, y}});
        };
        const auto circle = [&](float cx, float cy, float r, int sides) {
            for (int i = 0; i < sides; ++i)
            {
                const float a0 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(sides);
                const float a1 =
                    2.0f * std::numbers::pi_v<float> * static_cast<float>(i + 1) / static_cast<float>(sides);
                line(at(cx + r * std::cos(a0), cy + r * std::sin(a0)), at(cx + r * std::cos(a1), cy + r * std::sin(a1)),
                     stroke, color);
            }
        };

        switch (which)
        {
        case icon::none:
            break;
        case icon::check:
            path({{3.5f, 8.5f}, {6.5f, 11.5f}, {12.5f, 4.5f}});
            break;
        case icon::chevron_right:
            path({{6.0f, 4.0f}, {10.0f, 8.0f}, {6.0f, 12.0f}});
            break;
        case icon::chevron_left:
            path({{10.0f, 4.0f}, {6.0f, 8.0f}, {10.0f, 12.0f}});
            break;
        case icon::chevron_down:
            path({{4.0f, 6.0f}, {8.0f, 10.0f}, {12.0f, 6.0f}});
            break;
        case icon::chevron_up:
            path({{4.0f, 10.0f}, {8.0f, 6.0f}, {12.0f, 10.0f}});
            break;
        case icon::close:
            path({{4.0f, 4.0f}, {12.0f, 12.0f}});
            path({{12.0f, 4.0f}, {4.0f, 12.0f}});
            break;
        case icon::plus:
            path({{8.0f, 3.0f}, {8.0f, 13.0f}});
            path({{3.0f, 8.0f}, {13.0f, 8.0f}});
            break;
        case icon::minus:
            path({{3.0f, 8.0f}, {13.0f, 8.0f}});
            break;
        case icon::play:
            path({{5.0f, 3.0f}, {13.0f, 8.0f}, {5.0f, 13.0f}, {5.0f, 3.0f}});
            break;
        case icon::stop:
            box(4.0f, 4.0f, 8.0f, 8.0f);
            break;
        case icon::zoom_in:
        case icon::zoom_out:
            circle(7.0f, 7.0f, 4.5f, 20);
            path({{10.3f, 10.3f}, {14.0f, 14.0f}});
            path({{5.0f, 7.0f}, {9.0f, 7.0f}});
            if (which == icon::zoom_in)
                path({{7.0f, 5.0f}, {7.0f, 9.0f}});
            break;
        case icon::fit:
            path({{2.5f, 6.0f}, {2.5f, 2.5f}, {6.0f, 2.5f}});
            path({{10.0f, 2.5f}, {13.5f, 2.5f}, {13.5f, 6.0f}});
            path({{13.5f, 10.0f}, {13.5f, 13.5f}, {10.0f, 13.5f}});
            path({{6.0f, 13.5f}, {2.5f, 13.5f}, {2.5f, 10.0f}});
            break;
        case icon::grid:
            box(2.5f, 2.5f, 11.0f, 11.0f);
            path({{6.17f, 2.5f}, {6.17f, 13.5f}});
            path({{9.83f, 2.5f}, {9.83f, 13.5f}});
            path({{2.5f, 6.17f}, {13.5f, 6.17f}});
            path({{2.5f, 9.83f}, {13.5f, 9.83f}});
            break;
        case icon::folder:
            path({{2.0f, 4.0f}, {6.0f, 4.0f}, {7.5f, 5.5f}, {14.0f, 5.5f}, {14.0f, 12.5f}, {2.0f, 12.5f},
                  {2.0f, 4.0f}});
            break;
        case icon::file:
        case icon::sheet:
            path({{4.0f, 2.0f}, {9.5f, 2.0f}, {12.5f, 5.0f}, {12.5f, 14.0f}, {4.0f, 14.0f}, {4.0f, 2.0f}});
            path({{9.5f, 2.0f}, {9.5f, 5.0f}, {12.5f, 5.0f}});
            if (which == icon::sheet)
            {
                path({{6.0f, 8.0f}, {10.5f, 8.0f}});
                path({{6.0f, 10.5f}, {10.5f, 10.5f}});
            }
            break;
        case icon::component:
            box(4.5f, 4.0f, 7.0f, 8.0f);
            for (float y : {5.5f, 8.0f, 10.5f})
            {
                path({{2.0f, y}, {4.5f, y}});
                path({{11.5f, y}, {14.0f, y}});
            }
            break;
        case icon::net:
            path({{2.0f, 8.0f}, {14.0f, 8.0f}});
            path({{8.0f, 8.0f}, {8.0f, 13.0f}});
            fill(rect{at(6.6f, 6.6f).x(), at(6.6f, 6.6f).y(), 2.8f * unit, 2.8f * unit}, color, 1.4f * unit);
            break;
        case icon::wire:
            path({{2.0f, 12.0f}, {7.0f, 12.0f}, {7.0f, 4.0f}, {14.0f, 4.0f}});
            break;
        case icon::undo:
            path({{5.0f, 3.5f}, {2.5f, 6.0f}, {5.0f, 8.5f}});
            path({{2.5f, 6.0f}, {10.0f, 6.0f}, {12.5f, 8.5f}, {12.5f, 10.0f}, {10.0f, 12.5f}, {6.0f, 12.5f}});
            break;
        case icon::redo:
            path({{11.0f, 3.5f}, {13.5f, 6.0f}, {11.0f, 8.5f}});
            path({{13.5f, 6.0f}, {6.0f, 6.0f}, {3.5f, 8.5f}, {3.5f, 10.0f}, {6.0f, 12.5f}, {10.0f, 12.5f}});
            break;
        case icon::save:
            path({{2.5f, 2.5f}, {11.5f, 2.5f}, {13.5f, 4.5f}, {13.5f, 13.5f}, {2.5f, 13.5f}, {2.5f, 2.5f}});
            box(5.0f, 2.5f, 5.5f, 3.5f);
            box(4.5f, 9.0f, 7.0f, 4.5f);
            break;
        case icon::open:
            path({{2.0f, 12.5f}, {2.0f, 3.5f}, {6.0f, 3.5f}, {7.5f, 5.0f}, {12.0f, 5.0f}, {12.0f, 7.0f}});
            path({{2.0f, 12.5f}, {4.5f, 7.0f}, {14.5f, 7.0f}, {12.0f, 12.5f}, {2.0f, 12.5f}});
            break;
        case icon::settings:
            circle(8.0f, 8.0f, 2.3f, 14);
            for (int i = 0; i < 8; ++i)
            {
                const float a = static_cast<float>(i) * std::numbers::pi_v<float> / 4.0f;
                path({{8.0f + 4.2f * std::cos(a), 8.0f + 4.2f * std::sin(a)},
                      {8.0f + 6.2f * std::cos(a), 8.0f + 6.2f * std::sin(a)}});
            }
            circle(8.0f, 8.0f, 4.4f, 20);
            break;
        case icon::cube:
            path({{8.0f, 2.0f}, {13.5f, 5.0f}, {13.5f, 11.0f}, {8.0f, 14.0f}, {2.5f, 11.0f}, {2.5f, 5.0f},
                  {8.0f, 2.0f}});
            path({{2.5f, 5.0f}, {8.0f, 8.0f}, {13.5f, 5.0f}});
            path({{8.0f, 8.0f}, {8.0f, 14.0f}});
            break;
        case icon::pointer:
            path({{4.0f, 2.0f}, {4.0f, 13.0f}, {6.8f, 10.4f}, {8.8f, 14.5f}, {10.6f, 13.6f}, {8.6f, 9.6f},
                  {12.4f, 9.6f}, {4.0f, 2.0f}});
            break;
        case icon::tag:
            path({{2.0f, 8.0f}, {5.0f, 4.5f}, {14.0f, 4.5f}, {14.0f, 11.5f}, {5.0f, 11.5f}, {2.0f, 8.0f}});
            circle(5.6f, 8.0f, 0.9f, 8);
            break;
        case icon::power:
            // A supply arrow over a stem.
            path({{8.0f, 14.0f}, {8.0f, 2.5f}});
            path({{4.0f, 6.5f}, {8.0f, 2.5f}, {12.0f, 6.5f}});
            path({{4.5f, 14.0f}, {11.5f, 14.0f}});
            break;
        case icon::text:
            path({{3.0f, 4.5f}, {3.0f, 2.5f}, {13.0f, 2.5f}, {13.0f, 4.5f}});
            path({{8.0f, 2.5f}, {8.0f, 13.5f}});
            path({{5.5f, 13.5f}, {10.5f, 13.5f}});
            break;
        case icon::eye:
        case icon::eye_off:
            path({{1.5f, 8.0f}, {4.0f, 5.2f}, {8.0f, 3.8f}, {12.0f, 5.2f}, {14.5f, 8.0f}});
            path({{1.5f, 8.0f}, {4.0f, 10.8f}, {8.0f, 12.2f}, {12.0f, 10.8f}, {14.5f, 8.0f}});
            circle(8.0f, 8.0f, 2.2f, 12);
            if (which == icon::eye_off)
                path({{2.5f, 13.5f}, {13.5f, 2.5f}});
            break;
        case icon::route:
            path({{4.2f, 13.0f}, {7.5f, 13.0f}, {12.0f, 8.5f}, {12.0f, 4.2f}});
            circle(2.8f, 13.0f, 1.4f, 10);
            circle(12.0f, 2.8f, 1.4f, 10);
            break;
        case icon::via:
            circle(8.0f, 8.0f, 5.5f, 22);
            circle(8.0f, 8.0f, 2.2f, 12);
            break;
        case icon::zone:
            path({{2.5f, 3.0f}, {13.5f, 3.0f}, {13.5f, 13.0f}, {6.5f, 13.0f}, {2.5f, 9.0f}, {2.5f, 3.0f}});
            path({{7.0f, 3.0f}, {2.5f, 7.5f}});
            path({{11.5f, 3.0f}, {4.5f, 10.0f}});
            path({{13.5f, 5.5f}, {6.5f, 12.5f}});
            path({{13.5f, 10.0f}, {10.5f, 13.0f}});
            break;
        case icon::ruler:
            box(1.5f, 5.0f, 13.0f, 6.0f);
            for (float x : {4.0f, 6.5f, 9.0f, 11.5f})
                path({{x, 5.0f}, {x, x == 6.5f || x == 11.5f ? 8.5f : 7.0f}});
            break;
        case icon::layers:
            path({{8.0f, 2.0f}, {14.0f, 5.0f}, {8.0f, 8.0f}, {2.0f, 5.0f}, {8.0f, 2.0f}});
            path({{2.0f, 8.0f}, {8.0f, 11.0f}, {14.0f, 8.0f}});
            path({{2.0f, 11.0f}, {8.0f, 14.0f}, {14.0f, 11.0f}});
            break;
        case icon::target:
            path({{2.5f, 6.0f}, {2.5f, 2.5f}, {6.0f, 2.5f}});
            path({{10.0f, 2.5f}, {13.5f, 2.5f}, {13.5f, 6.0f}});
            path({{13.5f, 10.0f}, {13.5f, 13.5f}, {10.0f, 13.5f}});
            path({{6.0f, 13.5f}, {2.5f, 13.5f}, {2.5f, 10.0f}});
            box(6.0f, 6.0f, 4.0f, 4.0f);
            break;
        }
    }

} // namespace volt::ui
