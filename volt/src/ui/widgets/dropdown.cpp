#include <ui/widgets/dropdown.hpp>

#include <ui/painter.hpp>
#include <ui/screen.hpp>
#include <ui/theme.hpp>
#include <ui/widgets/menu.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{

    dropdown::dropdown(std::vector<std::string> items, int selected) : m_items(std::move(items)), m_selected(selected)
    {
        set_focusable(true);
        m_selected = std::clamp(m_selected, -1, static_cast<int>(m_items.size()) - 1);
    }

    void dropdown::set_items(std::vector<std::string> items)
    {
        close();
        m_items = std::move(items);
        m_selected = std::clamp(m_selected, -1, static_cast<int>(m_items.size()) - 1);
        invalidate_layout();
    }

    void dropdown::set_selected(int index)
    {
        index = std::clamp(index, -1, static_cast<int>(m_items.size()) - 1);
        if (index == m_selected)
            return;
        m_selected = index;
        invalidate();
    }

    const std::string &dropdown::selected_text() const
    {
        static const std::string none;
        return m_selected >= 0 ? m_items[static_cast<std::size_t>(m_selected)] : none;
    }

    void dropdown::choose(int index)
    {
        if (index == m_selected || index < 0 || index >= static_cast<int>(m_items.size()))
            return;
        m_selected = index;
        invalidate();
        changed.emit(index);
    }

    void dropdown::open()
    {
        if (m_list != nullptr || owner() == nullptr || m_items.empty())
            return;

        std::vector<menu_item> entries;
        for (std::size_t i = 0; i < m_items.size(); ++i)
        {
            menu_item item;
            item.text = m_items[i];
            const int index = static_cast<int>(i);
            item.action = [this, index] { choose(index); };
            item.checked = [this, index] { return m_selected == index; };
            entries.push_back(std::move(item));
        }

        popup_options options;
        options.anchor = bounds().inset(insets::symmetric(0.0f, -2.0f));
        options.placement = placement::below;
        options.match_anchor_width = true;
        options.owner = this;
        options.closed = [this] {
            m_list = nullptr;
            invalidate();
        };
        m_list = &owner()->open(std::make_unique<menu>(std::move(entries)), std::move(options));
        m_list->set_hot(m_selected);
        invalidate();
    }

    void dropdown::close()
    {
        if (m_list != nullptr && owner() != nullptr)
            owner()->close_popup(*m_list);
        m_list = nullptr;
    }

    math::vec2f dropdown::on_measure(const constraints &)
    {
        const ui::theme &t = theme();
        float widest = 0.0f;
        for (const std::string &item : m_items)
            widest = std::max(widest, fonts().measure(item, t.body_style()));
        return {std::ceil(std::max(widest + 2.0f * t.control_padding + 20.0f, 100.0f)), t.control_height};
    }

    void dropdown::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        const bool enabled = is_enabled();

        const math::vec4f fill = !enabled              ? t.control.disabled
                                 : is_open()           ? t.control.pressed
                                 : hovered()           ? t.control.hover
                                                       : t.control.normal;
        p.frame(b, fill, is_open() ? t.accent : t.control_border, t.radius);
        if (has_focus() && owner()->focus_visible())
            p.focus_ring(b, t.radius);

        const math::vec4f ink = enabled ? t.text : t.text_disabled;
        p.text({b.x + t.control_padding, b.y, b.width - 2.0f * t.control_padding - 16.0f, b.height}, selected_text(),
               t.body_style(), ink);
        const rect chevron{b.right() - t.control_padding - 12.0f, b.y + (b.height - 12.0f) * 0.5f, 12.0f, 12.0f};
        p.draw_icon(icon::chevron_down, chevron, enabled ? t.text_dim : t.text_disabled);
    }

    bool dropdown::on_pointer_down(const pointer_event &e)
    {
        if (e.button != mouse_button::left)
            return false;
        if (is_open())
            close();
        else
            open();
        return true;
    }

    bool dropdown::on_key(const key_event &e)
    {
        if (e.action == action::release)
            return false;
        const modifiers held = e.mods & held_modifiers;

        if ((held == modifiers::alt && e.key == key::down) ||
            (held == modifiers::none && (e.key == key::space || e.key == key::enter || e.key == key::f4)))
        {
            open();
            return true;
        }
        if (held != modifiers::none)
            return false;
        if (e.key == key::down)
        {
            choose(std::min(m_selected + 1, static_cast<int>(m_items.size()) - 1));
            return true;
        }
        if (e.key == key::up)
        {
            choose(std::max(m_selected - 1, 0));
            return true;
        }
        return false;
    }

} // namespace volt::ui
