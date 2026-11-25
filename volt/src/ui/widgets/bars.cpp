#include <ui/widgets/bars.hpp>

#include <ui/theme.hpp>
#include <ui/widgets/image_view.hpp>
#include <ui/widgets/panel.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{

    // ---- image_view (small enough to live here) ---------------------------------

    rect image_view::fitted(const rect &area, float scale) const noexcept
    {
        if (m_pixels.x() <= 0.0f || m_pixels.y() <= 0.0f || area.width <= 0.0f || area.height <= 0.0f)
            return {area.x, area.y, 0.0f, 0.0f};

        // The image at one image pixel per screen pixel, in logical units,
        // then scaled to fit.
        const float natural_width = m_pixels.x() / scale;
        const float natural_height = m_pixels.y() / scale;
        float factor = std::min(area.width / natural_width, area.height / natural_height);
        if (!m_enlarge)
            factor = std::min(factor, 1.0f);

        // Whole pixels, so that an image shown at its own size is not
        // resampled half a pixel off.
        const float width = std::max(std::round(natural_width * factor * scale), 1.0f) / scale;
        const float height = std::max(std::round(natural_height * factor * scale), 1.0f) / scale;
        const float x = std::round((area.x + (area.width - width) * 0.5f) * scale) / scale;
        const float y = std::round((area.y + (area.height - height) * 0.5f) * scale) / scale;
        return {x, y, width, height};
    }

    void image_view::on_paint(painter &p)
    {
        if (m_fit)
        {
            const rect area = fitted(bounds(), p.scale());
            if (area.width <= 0.0f || area.height <= 0.0f)
                return;

            if (m_checkerboard)
            {
                // Two greys, in the working space, that stay mid grey on
                // either theme. The squares are cut at the image's edges.
                const math::vec4f light{0.16f, 0.16f, 0.17f, 1.0f};
                const math::vec4f dark{0.08f, 0.08f, 0.09f, 1.0f};
                const float square = 8.0f;
                p.fill(area, light);
                for (float y = 0.0f; y < area.height; y += square)
                {
                    const int row = static_cast<int>(y / square);
                    for (float x = (row % 2 == 0) ? square : 0.0f; x < area.width; x += 2.0f * square)
                    {
                        p.fill({area.x + x, area.y + y, std::min(square, area.width - x), std::min(square, area.height - y)},
                               dark);
                    }
                }
            }
            if (m_image.valid())
                p.image(area, m_image, m_tint, m_radius);
            return;
        }

        const rect area{bounds().x + (bounds().width - m_size.x()) * 0.5f,
                        bounds().y + (bounds().height - m_size.y()) * 0.5f, m_size.x(), m_size.y()};
        if (m_image.valid())
            p.image(area, m_image, m_tint, m_radius);
        else
            p.draw_icon(m_icon, area, m_tinted ? m_tint : (is_enabled() ? theme().text : theme().text_disabled));
    }

    // ---- toolbar ------------------------------------------------------------------

    toolbar::toolbar() : row(2.0f, insets::symmetric(6.0f, 4.0f))
    {
        set_cross_align(align::center);
    }

    button &toolbar::add_tool(ui::icon icon, std::string tooltip, std::string text)
    {
        button &tool = emplace<button>(std::move(text), icon, button_kind::flat);
        tool.set_tooltip(std::move(tooltip));
        tool.set_focusable(false); // tools do not take the focus from the document
        return tool;
    }

    void toolbar::add_separator()
    {
        separator &line = emplace<separator>(orientation::vertical, 4.0f);
        line.layout().cross = align::stretch;
    }

    void toolbar::add_spacer()
    {
        emplace<spacer>();
    }

    math::vec2f toolbar::on_measure(const constraints &limits)
    {
        math::vec2f size = row::on_measure(limits);
        size[1] = std::max(size.y(), theme().toolbar_height);
        return size;
    }

    void toolbar::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        p.fill(b, t.panel);
        p.fill({b.x, b.bottom() - 1.0f, b.width, 1.0f}, t.separator);
        paint_children(p);
    }

    // ---- status_bar -------------------------------------------------------------------

    status_bar::status_bar() : row(8.0f, insets::symmetric(10.0f, 0.0f))
    {
        set_cross_align(align::stretch);
    }

    label &status_bar::add_segment(std::string text, float width)
    {
        label &segment = emplace<label>(std::move(text), text_role::dim);
        segment.set_size(text_size::small);
        if (width >= 0.0f)
            segment.layout().width = width;
        return segment;
    }

    void status_bar::add_separator()
    {
        emplace<separator>(orientation::vertical, 2.0f);
    }

    void status_bar::add_spacer()
    {
        emplace<spacer>();
    }

    math::vec2f status_bar::on_measure(const constraints &limits)
    {
        math::vec2f size = row::on_measure(limits);
        size[1] = std::max(size.y(), theme().status_bar_height);
        return size;
    }

    void status_bar::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        p.fill(b, t.panel);
        p.fill({b.x, b.y, b.width, 1.0f}, t.separator);
        paint_children(p);
    }

} // namespace volt::ui
