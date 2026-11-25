#include <ui/widgets/panel.hpp>

#include <ui/painter.hpp>
#include <ui/theme.hpp>

#include <cmath>

namespace volt::ui
{

    math::vec4f surface_color(const theme &t, surface s) noexcept
    {
        switch (s)
        {
        case surface::background:
            return t.background;
        case surface::panel:
            return t.panel;
        case surface::header:
            return t.panel_header;
        case surface::raised:
            return t.raised;
        case surface::field:
            return t.field;
        case surface::none:
            break;
        }
        return {0.0f, 0.0f, 0.0f, 0.0f};
    }

    // ---- panel ------------------------------------------------------------------

    panel::panel(std::string title, ui::surface surface, float spacing, insets padding)
        : box(orientation::vertical, spacing, padding), m_title(std::move(title)), m_surface(surface)
    {
    }

    void panel::set_title(std::string title)
    {
        m_title = std::move(title);
        invalidate_layout();
    }

    void panel::set_surface(ui::surface surface)
    {
        m_surface = surface;
        invalidate();
    }

    void panel::set_border(bool border)
    {
        m_border = border;
        invalidate();
    }

    void panel::set_edges(bool top, bool bottom, bool left, bool right)
    {
        m_edges[0] = top;
        m_edges[1] = bottom;
        m_edges[2] = left;
        m_edges[3] = right;
        invalidate();
    }

    float panel::header_height() const
    {
        return m_title.empty() ? 0.0f : theme().row_height + 6.0f;
    }

    math::vec2f panel::on_measure(const constraints &limits)
    {
        const float header = header_height();
        constraints inner = limits;
        inner.max[1] = std::max(0.0f, inner.max.y() - header);
        inner.min[1] = std::max(0.0f, inner.min.y() - header);
        math::vec2f size = box::on_measure(inner);
        size[1] += header;
        if (!m_title.empty())
        {
            const float title = fonts().measure(m_title, theme().small_style());
            size[0] = std::max(size.x(), title + 2.0f * std::max(padding().left, 8.0f));
        }
        return size;
    }

    void panel::on_arrange(const rect &bounds)
    {
        const float header = header_height();
        box::on_arrange({bounds.x, bounds.y + header, bounds.width, std::max(0.0f, bounds.height - header)});
    }

    void panel::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        const float radius = m_border ? t.radius : 0.0f;

        if (m_border)
            p.frame(b, surface_color(t, m_surface), t.border, radius);
        else
            p.fill(b, surface_color(t, m_surface));

        if (!m_title.empty())
        {
            const float header = header_height();
            const rect strip{b.x, b.y, b.width, header};
            if (!m_border)
                p.fill(strip, t.panel_header);
            p.fill({b.x, strip.bottom() - 1.0f, b.width, 1.0f}, t.separator);
            const float inset = std::max(padding().left, 8.0f);
            p.text({b.x + inset, b.y, b.width - 2.0f * inset, header - 1.0f}, m_title, t.small_style(), t.text_dim);
        }

        const float line = 1.0f;
        if (m_edges[0])
            p.fill({b.x, b.y, b.width, line}, t.separator);
        if (m_edges[1])
            p.fill({b.x, b.bottom() - line, b.width, line}, t.separator);
        if (m_edges[2])
            p.fill({b.x, b.y, line, b.height}, t.separator);
        if (m_edges[3])
            p.fill({b.right() - line, b.y, line, b.height}, t.separator);

        clip_scope clip(p, b);
        paint_children(p);
    }

    // ---- separator ---------------------------------------------------------------

    separator::separator(ui::orientation along, float margin) : m_along(along), m_margin(margin)
    {
        set_hit_testable(false);
    }

    math::vec2f separator::on_measure(const constraints &)
    {
        const float thickness = 1.0f + 2.0f * m_margin;
        return m_along == orientation::horizontal ? math::vec2f{0.0f, thickness} : math::vec2f{thickness, 0.0f};
    }

    void separator::on_paint(painter &p)
    {
        const rect &b = bounds();
        const math::vec4f color = theme().separator;
        if (m_along == orientation::horizontal)
            p.fill({b.x, std::floor(b.center().y()), b.width, 1.0f}, color);
        else
            p.fill({std::floor(b.center().x()), b.y + 3.0f, 1.0f, std::max(0.0f, b.height - 6.0f)}, color);
    }

} // namespace volt::ui
