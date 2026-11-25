#include <ui/widgets/slider.hpp>

#include <ui/painter.hpp>
#include <ui/screen.hpp>
#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{
    namespace
    {
        constexpr float thumb_radius = 7.0f;
    } // namespace

    slider::slider(double minimum, double maximum, double value, double step)
        : m_minimum(minimum), m_maximum(std::max(minimum, maximum)), m_value(minimum), m_step(step)
    {
        set_focusable(true);
        m_value = snapped(value);
    }

    double slider::snapped(double value) const noexcept
    {
        value = std::clamp(value, m_minimum, m_maximum);
        if (m_step > 0.0)
            value = std::clamp(m_minimum + std::round((value - m_minimum) / m_step) * m_step, m_minimum, m_maximum);
        return value;
    }

    void slider::set_value(double value)
    {
        value = snapped(value);
        if (value == m_value)
            return;
        m_value = value;
        invalidate();
    }

    void slider::set_range(double minimum, double maximum, double step)
    {
        m_minimum = minimum;
        m_maximum = std::max(minimum, maximum);
        m_step = step;
        m_value = snapped(m_value);
        invalidate();
    }

    void slider::set_formatter(std::function<std::string(double)> format)
    {
        m_format = std::move(format);
        invalidate_layout();
    }

    void slider::move_to(double value)
    {
        value = snapped(value);
        if (value == m_value)
            return;
        m_value = value;
        invalidate();
        changed.emit(m_value);
    }

    float slider::track_left() const noexcept
    {
        return bounds().x + thumb_radius;
    }

    float slider::track_right() const noexcept
    {
        return std::max(track_left(), bounds().right() - thumb_radius - label_width());
    }

    void slider::follow(float x)
    {
        const float span = track_right() - track_left();
        const double t = span > 0.0f ? std::clamp(static_cast<double>((x - track_left()) / span), 0.0, 1.0) : 0.0;
        move_to(m_minimum + t * (m_maximum - m_minimum));
    }

    math::vec2f slider::on_measure(const constraints &)
    {
        return {140.0f + label_width(), theme().control_height};
    }

    void slider::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        const bool enabled = is_enabled();

        const float range = static_cast<float>(m_maximum - m_minimum);
        const float fraction = range > 0.0f ? static_cast<float>(m_value - m_minimum) / range : 0.0f;
        const float left = track_left();
        const float right = track_right();
        const float middle = b.y + b.height * 0.5f;
        const float x = left + (right - left) * fraction;

        const float track = 4.0f;
        p.fill({left, middle - track * 0.5f, right - left, track}, t.track, track * 0.5f);
        p.fill({left, middle - track * 0.5f, x - left, track}, enabled ? t.accent : t.text_disabled, track * 0.5f);

        const bool hot = enabled && (hovered() || m_dragging);
        const rect thumb{x - thumb_radius, middle - thumb_radius, 2.0f * thumb_radius, 2.0f * thumb_radius};
        p.frame(thumb, enabled ? (hot ? t.text : t.control.normal) : t.control.disabled,
                enabled ? t.accent : t.control_border, thumb_radius, 2.0f);
        if (has_focus() && owner()->focus_visible())
            p.focus_ring(thumb, thumb_radius);

        if (m_format)
        {
            const float label_left = right + thumb_radius + 4.0f;
            p.text({label_left, b.y, b.right() - label_left, b.height}, m_format(m_value), t.body_style(),
                   enabled ? t.text_dim : t.text_disabled, align::end);
        }
    }

    bool slider::on_pointer_down(const pointer_event &e)
    {
        if (e.button != mouse_button::left)
            return false;
        m_dragging = true;
        follow(e.position.x());
        invalidate();
        return true;
    }

    void slider::on_pointer_move(const pointer_event &e)
    {
        if (m_dragging)
            follow(e.position.x());
    }

    bool slider::on_pointer_up(const pointer_event &e)
    {
        if (e.button != mouse_button::left)
            return false;
        m_dragging = false;
        invalidate();
        return true;
    }

    bool slider::on_key(const key_event &e)
    {
        if (e.action == action::release || (e.mods & held_modifiers) != modifiers::none)
            return false;

        const double range = m_maximum - m_minimum;
        const double small = m_step > 0.0 ? m_step : range / 100.0;
        const double large = std::max(small, range / 10.0);
        switch (e.key)
        {
        case key::left:
        case key::down:
            move_to(m_value - small);
            return true;
        case key::right:
        case key::up:
            move_to(m_value + small);
            return true;
        case key::page_down:
            move_to(m_value - large);
            return true;
        case key::page_up:
            move_to(m_value + large);
            return true;
        case key::home:
            move_to(m_minimum);
            return true;
        case key::end:
            move_to(m_maximum);
            return true;
        default:
            return false;
        }
    }

} // namespace volt::ui
