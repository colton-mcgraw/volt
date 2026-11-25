#include <ui/widgets/viewport.hpp>

#include <ui/painter.hpp>
#include <ui/screen.hpp>
#include <ui/theme.hpp>

#include <cmath>

namespace volt::ui
{

    viewport::viewport(texture_id image) : m_image(image)
    {
        set_focusable(true);
    }

    void viewport::set_image(texture_id image)
    {
        if (image == m_image)
            return;
        m_image = image;
        invalidate();
    }

    void viewport::set_radius(float radius)
    {
        m_radius = radius;
        invalidate();
    }

    math::vec2f viewport::to_pixels(const math::vec2f &point) const noexcept
    {
        const float scale = owner() != nullptr ? owner()->scale() : 1.0f;
        // From the snapped corner, as the image is drawn.
        return {point.x() * scale - std::round(bounds().x * scale), point.y() * scale - std::round(bounds().y * scale)};
    }

    math::vec2f viewport::on_measure(const constraints &limits)
    {
        // Whatever it is given; a viewport has no size of its own.
        return {std::isfinite(limits.max.x()) ? limits.max.x() : 320.0f,
                std::isfinite(limits.max.y()) ? limits.max.y() : 240.0f};
    }

    void viewport::on_arrange(const rect &bounds)
    {
        const float scale = owner() != nullptr ? owner()->scale() : 1.0f;
        const math::vec2i pixels{static_cast<int>(std::round(bounds.right() * scale) - std::round(bounds.x * scale)),
                                 static_cast<int>(std::round(bounds.bottom() * scale) - std::round(bounds.y * scale))};
        if (pixels != m_pixels)
        {
            m_pixels = pixels;
            resized.emit(pixels);
        }
    }

    void viewport::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        if (m_image.valid())
            p.image(b, m_image, {1.0f, 1.0f, 1.0f, 1.0f}, m_radius);
        else
            p.fill(b, t.field, m_radius);
        if (!m_caption.empty())
        {
            const text_style style = t.small_style();
            const float width = std::ceil(fonts().measure(m_caption, style)) + 16.0f;
            const rect pill{b.x + 8.0f, b.y + 8.0f, width, t.row_height - 2.0f};
            p.fill(pill, {0.0f, 0.0f, 0.0f, 0.45f}, pill.height * 0.5f);
            p.text(pill, m_caption, style, {0.85f, 0.87f, 0.90f, 1.0f}, align::center);
        }
        if (has_focus() && owner()->focus_visible())
            p.focus_ring(b, m_radius);
    }

    void viewport::set_caption(std::string caption)
    {
        m_caption = std::move(caption);
        invalidate();
    }

    viewport_pointer viewport::translate(const pointer_event &e) const
    {
        const float scale = owner() != nullptr ? owner()->scale() : 1.0f;
        viewport_pointer v;
        v.position = to_pixels(e.position);
        v.delta = e.delta * scale;
        v.size = {static_cast<float>(m_pixels.x()), static_cast<float>(m_pixels.y())};
        v.button = e.button;
        v.buttons = e.buttons;
        v.mods = e.mods;
        v.clicks = e.clicks;
        return v;
    }

    void viewport::report(bool did)
    {
        if (!did)
            return;
        invalidate();
        changed.emit();
    }

    bool viewport::on_pointer_down(const pointer_event &e)
    {
        m_dragging = true;
        if (m_controller != nullptr)
            report(m_controller->pointer_down(translate(e)));
        return true; // captures, for the controller's drags
    }

    bool viewport::on_pointer_up(const pointer_event &e)
    {
        if (e.buttons == mouse_buttons::none)
            m_dragging = false;
        if (m_controller != nullptr)
            report(m_controller->pointer_up(translate(e)));
        return true;
    }

    void viewport::on_pointer_move(const pointer_event &e)
    {
        const viewport_pointer v = translate(e);
        m_pointer = v.position;
        m_has_pointer = bounds().contains(e.position);
        if (m_controller != nullptr)
            report(m_controller->pointer_move(v));
        if (m_has_pointer)
            pointer_moved.emit(v.position);
    }

    void viewport::on_pointer_leave()
    {
        m_has_pointer = false;
        pointer_left.emit();
    }

    bool viewport::on_click(const pointer_event &e)
    {
        if (e.button == mouse_button::right)
        {
            context_requested.emit(e.position);
            return true;
        }
        return false;
    }

    bool viewport::on_scroll(const scroll_input &e)
    {
        if (m_controller == nullptr)
            return false;
        pointer_event fake;
        fake.position = e.position;
        fake.mods = e.mods;
        const bool did = m_controller->scroll(e.offset, translate(fake));
        report(did);
        return true;
    }

    bool viewport::on_key(const key_event &e)
    {
        if (m_controller == nullptr || e.action == action::release)
            return false;
        const bool did = m_controller->key(e, {static_cast<float>(m_pixels.x()), static_cast<float>(m_pixels.y())});
        report(did);
        return did;
    }

    cursor_shape viewport::cursor(const math::vec2f &) const
    {
        return m_dragging ? cursor_shape::move : cursor_shape::crosshair;
    }

} // namespace volt::ui
