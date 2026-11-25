#include <ui/widgets/checkbox.hpp>

#include <ui/painter.hpp>
#include <ui/screen.hpp>
#include <ui/theme.hpp>

#include <cmath>

namespace volt::ui
{

    checkbox::checkbox(std::string text, bool checked) : m_text(std::move(text)), m_checked(checked)
    {
        set_focusable(true);
    }

    void checkbox::set_checked(bool checked)
    {
        if (checked == m_checked)
            return;
        m_checked = checked;
        invalidate();
    }

    void checkbox::set_text(std::string text)
    {
        m_text = std::move(text);
        invalidate_layout();
    }

    void checkbox::toggle()
    {
        if (!is_enabled())
            return;
        m_checked = !m_checked;
        invalidate();
        changed.emit(m_checked);
    }

    math::vec2f checkbox::on_measure(const constraints &)
    {
        const ui::theme &t = theme();
        float width = t.check_size;
        if (!m_text.empty())
            width += t.spacing + std::ceil(fonts().measure(m_text, t.body_style()));
        return {width, t.control_height - 4.0f};
    }

    void checkbox::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        const bool enabled = is_enabled();

        const rect square{b.x, std::round(b.y + (b.height - t.check_size) * 0.5f), t.check_size, t.check_size};
        if (m_checked)
        {
            const math::vec4f fill = !enabled ? t.control.disabled : hovered() ? t.accent_hover : t.check;
            p.fill(square, fill, t.small_radius);
            p.draw_icon(icon::check, square.inset(1.0f), enabled ? t.text_on_accent : t.text_disabled);
        }
        else
        {
            const math::vec4f fill = !enabled ? t.control.disabled : m_pressed ? t.control.pressed : t.field;
            const math::vec4f border = hovered() && enabled ? t.accent : t.control_border;
            p.frame(square, fill, border, t.small_radius);
        }
        if (has_focus() && owner()->focus_visible())
            p.focus_ring(square, t.small_radius);

        if (!m_text.empty())
        {
            const float x = square.right() + t.spacing;
            p.text({x, b.y, b.right() - x, b.height}, m_text, t.body_style(), enabled ? t.text : t.text_disabled);
        }
    }

    bool checkbox::on_pointer_down(const pointer_event &e)
    {
        if (e.button != mouse_button::left)
            return false;
        m_pressed = true;
        invalidate();
        return true;
    }

    bool checkbox::on_pointer_up(const pointer_event &e)
    {
        if (e.button != mouse_button::left || !m_pressed)
            return false;
        m_pressed = false;
        if (bounds().contains(e.position))
            toggle();
        invalidate();
        return true;
    }

    bool checkbox::on_key(const key_event &e)
    {
        if (e.action == action::press && e.key == key::space && (e.mods & held_modifiers) == modifiers::none)
        {
            toggle();
            return true;
        }
        return false;
    }

} // namespace volt::ui
