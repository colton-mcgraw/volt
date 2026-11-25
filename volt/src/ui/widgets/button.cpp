#include <ui/widgets/button.hpp>

#include <ui/screen.hpp>
#include <ui/theme.hpp>

#include <cmath>

namespace volt::ui
{

    button::button(std::string text, ui::icon icon, button_kind kind)
        : m_text(std::move(text)), m_icon(icon), m_kind(kind)
    {
        set_focusable(true);
    }

    void button::set_text(std::string text)
    {
        if (text == m_text)
            return;
        m_text = std::move(text);
        invalidate_layout();
    }

    void button::set_icon(ui::icon icon)
    {
        m_icon = icon;
        invalidate_layout();
    }

    void button::set_image(texture_id image, float size)
    {
        m_image = image;
        m_image_size = size;
        invalidate_layout();
    }

    void button::set_kind(button_kind kind)
    {
        m_kind = kind;
        invalidate();
    }

    void button::set_checked(bool checked)
    {
        if (checked == m_checked)
            return;
        m_checked = checked;
        invalidate();
    }

    void button::activate()
    {
        if (!is_enabled())
            return;
        if (m_toggle)
        {
            set_checked(!m_checked);
            toggled.emit(m_checked);
        }
        clicked.emit();
    }

    math::vec2f button::on_measure(const constraints &)
    {
        const ui::theme &t = theme();
        const bool has_icon = m_icon != icon::none || m_image.valid();
        const float icon_size = m_image.valid() ? m_image_size : t.icon_size;
        const float height = m_kind == button_kind::flat && m_text.empty() ? t.toolbar_height - 8.0f : t.control_height;

        if (m_text.empty())
            return {has_icon ? std::max(height, icon_size + 12.0f) : height, height};

        float width = std::ceil(fonts().measure(m_text, t.body_style())) + 2.0f * t.control_padding;
        if (has_icon)
            width += icon_size + t.spacing;
        if (m_kind != button_kind::flat)
            width = std::max(width, 72.0f);
        return {width, height};
    }

    void button::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        const bool enabled = is_enabled();
        const bool down = is_pressed() || (m_toggle && m_checked);
        const float hover = m_hover.value();

        math::vec4f fill;
        math::vec4f border = t.control_border;
        math::vec4f ink = enabled ? t.text : t.text_disabled;

        switch (m_kind)
        {
        case button_kind::primary:
            fill = !enabled ? t.control.disabled
                   : down   ? t.accent_pressed
                            : mix(t.accent, t.accent_hover, hover);
            border = faded(fill, 0.0f);
            ink = enabled ? t.text_on_accent : t.text_disabled;
            break;
        case button_kind::flat:
            fill = !enabled ? t.flat.disabled
                   : down   ? (m_toggle && m_checked ? faded(t.accent, 0.30f) : t.flat.pressed)
                            : mix(t.flat.normal, t.flat.hover, hover);
            border = m_toggle && m_checked ? faded(t.accent, 0.6f) : faded(border, 0.0f);
            if (m_toggle && m_checked && enabled)
                ink = t.text;
            break;
        case button_kind::normal:
            fill = !enabled ? t.control.disabled
                   : down   ? t.control.pressed
                            : mix(t.control.normal, t.control.hover, hover);
            if (m_toggle && m_checked)
                border = t.accent;
            break;
        }

        p.frame(b, fill, border, t.radius);
        if (has_focus() && owner()->focus_visible())
            p.focus_ring(b, t.radius);

        // Content: icon then text, centred together.
        const bool has_icon = m_icon != icon::none || m_image.valid();
        const float icon_size = m_image.valid() ? m_image_size : t.icon_size;
        const float text_width = m_text.empty() ? 0.0f : fonts().measure(m_text, t.body_style());
        const float content = (has_icon ? icon_size : 0.0f) + (has_icon && !m_text.empty() ? t.spacing : 0.0f) +
                              text_width;
        float x = std::round(b.x + std::max(t.control_padding * 0.5f, (b.width - content) * 0.5f));

        if (has_icon)
        {
            const rect icon_area{x, std::round(b.y + (b.height - icon_size) * 0.5f), icon_size, icon_size};
            if (m_image.valid())
                p.image(icon_area, m_image, enabled ? math::vec4f{1.0f, 1.0f, 1.0f, 1.0f} : faded(ink, 0.5f));
            else
                p.draw_icon(m_icon, icon_area, ink);
            x += icon_size + t.spacing;
        }
        if (!m_text.empty())
            p.text({x, b.y, b.right() - x, b.height}, m_text, t.body_style(), ink);
    }

    bool button::on_pointer_down(const pointer_event &e)
    {
        if (e.button != mouse_button::left)
            return false;
        m_pressed = true;
        m_inside = true;
        invalidate();
        return true;
    }

    void button::on_pointer_move(const pointer_event &e)
    {
        if (!m_pressed)
            return;
        const bool inside = bounds().contains(e.position);
        if (inside != m_inside)
        {
            m_inside = inside;
            invalidate();
        }
    }

    bool button::on_pointer_up(const pointer_event &e)
    {
        if (e.button != mouse_button::left || !m_pressed)
            return false;
        const bool activated = bounds().contains(e.position);
        m_pressed = false;
        m_inside = false;
        invalidate();
        if (activated)
            activate();
        return true;
    }

    void button::on_pointer_enter()
    {
        if (m_hover.set(1.0f))
            animate();
        invalidate();
    }

    void button::on_pointer_leave()
    {
        if (m_hover.set(0.0f))
            animate();
        invalidate();
    }

    bool button::on_animate(float seconds)
    {
        const bool more = m_hover.step(seconds, theme().transition);
        invalidate();
        return more;
    }

    bool button::on_key(const key_event &e)
    {
        if (e.action != action::press || (e.mods & held_modifiers) != modifiers::none)
            return false;
        if (e.key == key::space || e.key == key::enter || e.key == key::kp_enter)
        {
            activate();
            return true;
        }
        return false;
    }

} // namespace volt::ui
