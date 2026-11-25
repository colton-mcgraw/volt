#include <ui/widgets/menu.hpp>

#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{
    namespace
    {
        constexpr float edge = 4.0f;           // above the first item and below the last
        constexpr float separator_height = 9.0f;
        constexpr float check_column = 26.0f;  // check marks and icons
        constexpr float arrow_column = 20.0f;  // submenu arrows
        constexpr float shortcut_gap = 28.0f;

        float item_height(const theme &t) noexcept
        {
            return t.row_height + 2.0f;
        }
    } // namespace

    menu::menu(std::vector<menu_item> items, menu *parent) : m_items(std::move(items)), m_parent(parent)
    {
        set_focusable(true);

        // Texts that depend on the moment are worked out as the menu opens.
        for (menu_item &item : m_items)
        {
            if (item.text_of)
                item.text = item.text_of();
        }
    }

    bool menu::usable(int index) const
    {
        if (index < 0 || index >= static_cast<int>(m_items.size()))
            return false;
        const menu_item &item = m_items[static_cast<std::size_t>(index)];
        return !item.is_separator && (!item.enabled || item.enabled());
    }

    int menu::step(int from, int direction) const
    {
        const int count = static_cast<int>(m_items.size());
        if (count == 0)
            return -1;
        int at = from;
        for (int tries = 0; tries < count; ++tries)
        {
            at = at < 0 ? (direction > 0 ? 0 : count - 1) : (at + direction + count) % count;
            if (usable(at))
                return at;
        }
        return from;
    }

    void menu::set_hot(int index)
    {
        if (index == m_hot)
            return;
        m_hot = index;
        invalidate();
    }

    menu &menu::root()
    {
        menu *m = this;
        while (m->m_parent != nullptr)
            m = m->m_parent;
        return *m;
    }

    rect menu::item_rect(std::size_t index) const
    {
        const ui::theme &t = theme();
        float y = bounds().y + edge;
        for (std::size_t i = 0; i < index && i < m_items.size(); ++i)
            y += m_items[i].is_separator ? separator_height : item_height(t);
        const float height = index < m_items.size() && m_items[index].is_separator ? separator_height : item_height(t);
        return {bounds().x, y, bounds().width, height};
    }

    int menu::item_at(const math::vec2f &point) const
    {
        if (!bounds().contains(point))
            return -1;
        for (std::size_t i = 0; i < m_items.size(); ++i)
        {
            if (item_rect(i).contains(point))
                return static_cast<int>(i);
        }
        return -1;
    }

    math::vec2f menu::on_measure(const constraints &)
    {
        const ui::theme &t = theme();
        float text_width = 0.0f;
        float shortcut_width = 0.0f;
        float height = 2.0f * edge;
        bool arrows = false;
        for (const menu_item &item : m_items)
        {
            if (item.is_separator)
            {
                height += separator_height;
                continue;
            }
            height += item_height(t);
            text_width = std::max(text_width, fonts().measure(item.text, t.body_style()));
            if (!item.command.empty() && owner() != nullptr)
            {
                const std::string shortcut = owner()->shortcut_text(item.command);
                if (!shortcut.empty())
                    shortcut_width = std::max(shortcut_width, fonts().measure(shortcut, t.body_style()));
            }
            arrows = arrows || !item.submenu.empty();
        }

        float width = check_column + text_width + 12.0f;
        if (shortcut_width > 0.0f)
            width += shortcut_gap + shortcut_width;
        width += arrows ? arrow_column : 8.0f;
        return {std::ceil(std::max(width, 140.0f)), height};
    }

    void menu::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        p.frame(b, t.raised, t.border, t.radius);

        for (std::size_t i = 0; i < m_items.size(); ++i)
        {
            const menu_item &item = m_items[i];
            const rect r = item_rect(i);
            if (item.is_separator)
            {
                p.fill({r.x + 8.0f, std::floor(r.center().y()), r.width - 16.0f, 1.0f}, t.separator);
                continue;
            }

            const bool enabled = usable(static_cast<int>(i));
            const bool hot = static_cast<int>(i) == m_hot && enabled;
            if (hot)
                p.fill(r.inset(insets::symmetric(4.0f, 1.0f)), t.selection, t.small_radius);

            const math::vec4f ink = enabled ? t.text : t.text_disabled;
            const rect check{r.x + 6.0f, r.y, check_column - 8.0f, r.height};
            if (item.checked && item.checked())
                p.draw_icon(icon::check, check.inset(insets::symmetric(0.0f, 3.0f)), enabled ? t.accent : ink);
            else if (item.icon != icon::none)
                p.draw_icon(item.icon, check.inset(insets::symmetric(1.0f, 4.0f)), enabled ? t.text_dim : ink);

            const float text_left = r.x + check_column;
            p.text({text_left, r.y, r.width - check_column - arrow_column, r.height}, item.text, t.body_style(), ink);

            if (!item.command.empty())
            {
                const std::string shortcut = owner()->shortcut_text(item.command);
                if (!shortcut.empty())
                    p.text({text_left, r.y, r.width - check_column - arrow_column + 8.0f, r.height}, shortcut,
                           t.body_style(), enabled ? t.text_dim : t.text_disabled, align::end);
            }
            if (!item.submenu.empty())
            {
                const rect arrow{r.right() - arrow_column, r.y + (r.height - 12.0f) * 0.5f, 12.0f, 12.0f};
                p.draw_icon(icon::chevron_right, arrow, ink);
            }
        }
    }

    void menu::open_submenu(int index, bool focus_first)
    {
        if (m_child != nullptr && m_child_index == index)
        {
            if (focus_first)
            {
                m_child->focus(focus_reason::keyboard);
                m_child->set_hot(m_child->step(-1, 1));
            }
            return;
        }
        close_submenu();
        if (!usable(index) || owner() == nullptr)
            return;

        popup_options options;
        options.anchor = item_rect(static_cast<std::size_t>(index)).inset(insets::symmetric(-2.0f, edge));
        options.anchor.y -= edge;
        options.placement = placement::right;
        options.take_focus = focus_first;
        options.owner = this;
        options.closed = [this, index] {
            if (m_child_index == index)
            {
                m_child = nullptr;
                m_child_index = -1;
            }
        };

        m_child_index = index;
        m_child = &owner()->open(std::make_unique<menu>(m_items[static_cast<std::size_t>(index)].submenu, this),
                                 std::move(options));
        if (focus_first)
            m_child->set_hot(m_child->step(-1, 1));
    }

    void menu::close_submenu()
    {
        if (m_child != nullptr && owner() != nullptr)
            owner()->close_popup(*m_child);
        m_child = nullptr;
        m_child_index = -1;
    }

    void menu::activate(int index)
    {
        if (!usable(index) || owner() == nullptr)
            return;
        const menu_item &item = m_items[static_cast<std::size_t>(index)];
        if (!item.submenu.empty())
        {
            open_submenu(index, true);
            return;
        }

        // Copied out first: closing destroys this menu, and the item.
        const std::function<void()> action = item.action;
        const std::string command = item.command;
        screen &s = *owner();
        s.close_popup(root());

        if (action)
            action();
        else if (!command.empty())
            s.run_command(command);
    }

    bool menu::on_pointer_down(const pointer_event &e)
    {
        if (e.button != mouse_button::left && e.button != mouse_button::right)
            return false;
        m_pressed = item_at(e.position);
        return true;
    }

    bool menu::on_pointer_up(const pointer_event &e)
    {
        const int index = item_at(e.position);
        const int pressed = std::exchange(m_pressed, -1);
        if (index >= 0 && index == pressed)
            activate(index);
        return true;
    }

    void menu::on_pointer_move(const pointer_event &e)
    {
        const int index = item_at(e.position);
        if (index < 0)
            return;
        set_hot(usable(index) ? index : -1);
        if (usable(index) && !m_items[static_cast<std::size_t>(index)].submenu.empty())
            open_submenu(index, false);
        else
            close_submenu();
    }

    bool menu::on_key(const key_event &e)
    {
        if (e.action == action::release || (e.mods & held_modifiers) != modifiers::none)
            return false;

        switch (e.key)
        {
        case key::down:
            close_submenu();
            set_hot(step(m_hot, 1));
            return true;
        case key::up:
            close_submenu();
            set_hot(step(m_hot, -1));
            return true;
        case key::home:
            set_hot(step(-1, 1));
            return true;
        case key::end:
            set_hot(step(-1, -1));
            return true;
        case key::right:
            if (usable(m_hot) && !m_items[static_cast<std::size_t>(m_hot)].submenu.empty())
                open_submenu(m_hot, true);
            else if (m_parent == nullptr)
                navigate.emit(1);
            return true;
        case key::left:
            if (m_parent != nullptr)
                owner()->close_popup(*this);
            else
                navigate.emit(-1);
            return true;
        case key::enter:
        case key::kp_enter:
        case key::space:
            activate(m_hot);
            return true;
        default:
            return false;
        }
    }

    menu &open_menu(screen &screen, std::vector<menu_item> items, const rect &anchor, placement where,
                    widget *owner, std::function<void()> closed)
    {
        popup_options options;
        options.anchor = anchor;
        options.placement = where;
        options.owner = owner;
        options.closed = std::move(closed);
        return screen.open(std::make_unique<menu>(std::move(items)), std::move(options));
    }

    menu &open_context_menu(screen &screen, std::vector<menu_item> items, const math::vec2f &point)
    {
        return open_menu(screen, std::move(items), {point.x(), point.y(), 0.0f, 0.0f}, placement::point);
    }

    // ---- menu_bar ------------------------------------------------------------------

    menu_bar::menu_bar()
    {
        set_tracks_pointer(true);
    }

    void menu_bar::add_menu(std::string title, std::vector<menu_item> items)
    {
        m_menus.push_back({std::move(title), std::move(items), 0.0f});
        invalidate_layout();
    }

    rect menu_bar::title_rect(std::size_t index) const
    {
        float x = bounds().x + 4.0f;
        for (std::size_t i = 0; i < index && i < m_menus.size(); ++i)
            x += m_menus[i].width;
        const float width = index < m_menus.size() ? m_menus[index].width : 0.0f;
        return {x, bounds().y + 2.0f, width, bounds().height - 4.0f};
    }

    int menu_bar::title_at(const math::vec2f &point) const
    {
        for (std::size_t i = 0; i < m_menus.size(); ++i)
        {
            if (title_rect(i).contains(point))
                return static_cast<int>(i);
        }
        return -1;
    }

    math::vec2f menu_bar::on_measure(const constraints &)
    {
        const ui::theme &t = theme();
        float width = 8.0f;
        for (entry &e : m_menus)
        {
            e.width = std::ceil(fonts().measure(e.title, t.body_style())) + 2.0f * 10.0f;
            width += e.width;
        }
        return {width, t.menu_bar_height};
    }

    void menu_bar::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        for (std::size_t i = 0; i < m_menus.size(); ++i)
        {
            const rect r = title_rect(i);
            const bool open = static_cast<int>(i) == m_open_index;
            if (open)
                p.fill(r, t.selection, t.small_radius);
            else if (static_cast<int>(i) == m_hot)
                p.fill(r, t.flat.hover, t.small_radius);
            p.text(r, m_menus[i].title, t.body_style(), t.text, align::center);
        }
    }

    void menu_bar::open(int index, bool focus_first)
    {
        if (index < 0 || index >= static_cast<int>(m_menus.size()) || owner() == nullptr)
            return;
        close();

        m_open_index = index;
        const rect anchor = title_rect(static_cast<std::size_t>(index));
        m_open = &open_menu(*owner(), m_menus[static_cast<std::size_t>(index)].items, {anchor.x, anchor.y, anchor.width,
                                                                                        anchor.height + 2.0f},
                            placement::below, this, [this, index] {
                                if (m_open_index == index)
                                {
                                    m_open = nullptr;
                                    m_open_index = -1;
                                    invalidate();
                                }
                            });
        const int count = static_cast<int>(m_menus.size());
        m_open->navigate.connect([this, count](int direction) {
            const int next = (m_open_index + direction + count) % count;
            open(next, true);
        });
        if (focus_first)
            m_open->set_hot(0);
        invalidate();
    }

    void menu_bar::close()
    {
        if (m_open != nullptr && owner() != nullptr)
            owner()->close_popup(*m_open);
        m_open = nullptr;
        m_open_index = -1;
        invalidate();
    }

    bool menu_bar::on_pointer_down(const pointer_event &e)
    {
        if (e.button != mouse_button::left)
            return false;
        const int index = title_at(e.position);
        if (index < 0)
            return false;
        if (index == m_open_index)
            close();
        else
            open(index);
        return true;
    }

    void menu_bar::on_pointer_move(const pointer_event &e)
    {
        const int index = title_at(e.position);
        if (index != m_hot)
        {
            m_hot = index;
            invalidate();
        }
        if (m_open_index >= 0 && index >= 0 && index != m_open_index)
            open(index);
    }

    void menu_bar::on_pointer_leave()
    {
        if (m_hot >= 0)
            invalidate();
        m_hot = -1;
    }

} // namespace volt::ui
