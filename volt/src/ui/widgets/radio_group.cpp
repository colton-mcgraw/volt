#include <ui/widgets/radio_group.hpp>

#include <ui/painter.hpp>
#include <ui/screen.hpp>
#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{

    radio_group::radio_group(std::vector<std::string> options, int selected)
        : m_options(std::move(options)), m_selected(selected)
    {
        set_focusable(true);
    }

    void radio_group::set_options(std::vector<std::string> options)
    {
        m_options = std::move(options);
        m_selected = std::clamp(m_selected, -1, static_cast<int>(m_options.size()) - 1);
        invalidate_layout();
    }

    void radio_group::set_selected(int index)
    {
        index = std::clamp(index, -1, static_cast<int>(m_options.size()) - 1);
        if (index == m_selected)
            return;
        m_selected = index;
        invalidate();
    }

    void radio_group::choose(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_options.size()) || index == m_selected)
            return;
        m_selected = index;
        invalidate();
        changed.emit(index);
    }

    float radio_group::row_height() const
    {
        return theme().control_height - 4.0f;
    }

    int radio_group::option_at(const math::vec2f &point) const
    {
        if (!bounds().contains(point))
            return -1;
        const int index = static_cast<int>((point.y() - bounds().y) / row_height());
        return index < static_cast<int>(m_options.size()) ? index : -1;
    }

    math::vec2f radio_group::on_measure(const constraints &)
    {
        const ui::theme &t = theme();
        float width = 0.0f;
        for (const std::string &option : m_options)
            width = std::max(width, fonts().measure(option, t.body_style()));
        return {std::ceil(t.check_size + t.spacing + width), row_height() * static_cast<float>(m_options.size())};
    }

    void radio_group::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        const bool enabled = is_enabled();
        const float height = row_height();

        for (std::size_t i = 0; i < m_options.size(); ++i)
        {
            const float top = b.y + height * static_cast<float>(i);
            const rect circle{b.x, std::round(top + (height - t.check_size) * 0.5f), t.check_size, t.check_size};
            const bool on = static_cast<int>(i) == m_selected;
            const bool hot = static_cast<int>(i) == m_hot && enabled;

            if (on)
            {
                p.fill(circle, enabled ? (hot ? t.accent_hover : t.check) : t.control.disabled, t.check_size * 0.5f);
                p.fill(circle.inset(t.check_size * 0.3f), enabled ? t.text_on_accent : t.text_disabled,
                       t.check_size * 0.5f);
            }
            else
            {
                p.frame(circle, enabled ? t.field : t.control.disabled, hot ? t.accent : t.control_border,
                        t.check_size * 0.5f);
            }
            if (on && has_focus() && owner()->focus_visible())
                p.focus_ring(circle, t.check_size * 0.5f);

            const float x = circle.right() + t.spacing;
            p.text({x, top, b.right() - x, height}, m_options[i], t.body_style(), enabled ? t.text : t.text_disabled);
        }
    }

    bool radio_group::on_pointer_down(const pointer_event &e)
    {
        if (e.button != mouse_button::left)
            return false;
        choose(option_at(e.position));
        return true;
    }

    void radio_group::on_pointer_move(const pointer_event &e)
    {
        const int hot = option_at(e.position);
        if (hot != m_hot)
        {
            m_hot = hot;
            invalidate();
        }
    }

    void radio_group::on_pointer_leave()
    {
        m_hot = -1;
        invalidate();
    }

    bool radio_group::on_key(const key_event &e)
    {
        if (e.action == action::release || (e.mods & held_modifiers) != modifiers::none)
            return false;
        const int count = static_cast<int>(m_options.size());
        if (count == 0)
            return false;
        if (e.key == key::down || e.key == key::right)
        {
            choose(std::min(m_selected + 1, count - 1));
            return true;
        }
        if (e.key == key::up || e.key == key::left)
        {
            choose(std::max(m_selected - 1, 0));
            return true;
        }
        return false;
    }

} // namespace volt::ui
