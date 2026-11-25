#include <ui/widgets/tab_view.hpp>

#include <ui/painter.hpp>
#include <ui/screen.hpp>
#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{

    tab_view::tab_view()
    {
        set_tracks_pointer(true);
    }

    void tab_view::add_page(std::string title, std::unique_ptr<widget> page)
    {
        m_titles.push_back(std::move(title));
        page->set_visible(m_titles.size() == 1);
        add(std::move(page));
        if (m_selected < 0)
            m_selected = 0;
    }

    void tab_view::select(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_titles.size()) || index == m_selected)
            return;
        m_selected = index;
        for (std::size_t i = 0; i < child_count(); ++i)
            child(i).set_visible(static_cast<int>(i) == index);
        invalidate_layout();
    }

    void tab_view::choose(int index)
    {
        const int before = m_selected;
        select(index);
        if (m_selected != before)
            changed.emit(m_selected);
    }

    rect tab_view::tab_rect(std::size_t index) const
    {
        const ui::theme &t = theme();
        float x = bounds().x + 4.0f;
        for (std::size_t i = 0; i < index && i < m_widths.size(); ++i)
            x += m_widths[i] + 2.0f;
        const float width = index < m_widths.size() ? m_widths[index] : 0.0f;
        return {x, bounds().y + 4.0f, width, t.tab_height - 4.0f};
    }

    int tab_view::tab_at(const math::vec2f &point) const
    {
        for (std::size_t i = 0; i < m_titles.size(); ++i)
        {
            if (tab_rect(i).contains(point))
                return static_cast<int>(i);
        }
        return -1;
    }

    math::vec2f tab_view::on_measure(const constraints &limits)
    {
        const ui::theme &t = theme();
        m_widths.clear();
        float tabs = 4.0f;
        for (const std::string &title : m_titles)
        {
            m_widths.push_back(std::ceil(fonts().measure(title, t.body_style())) + 2.0f * t.control_padding + 4.0f);
            tabs += m_widths.back() + 2.0f;
        }

        constraints inner = constraints::loose(limits.max);
        inner.max[1] = std::max(0.0f, inner.max.y() - t.tab_height);
        math::vec2f page{};
        for (const auto &c : children())
        {
            if (c->visible())
                page = c->measure(inner);
        }
        return {std::max(tabs, page.x()), t.tab_height + page.y()};
    }

    void tab_view::on_arrange(const rect &bounds)
    {
        const float header = theme().tab_height;
        const rect content{bounds.x, bounds.y + header, bounds.width, std::max(0.0f, bounds.height - header)};
        for (const auto &c : children())
            c->arrange(content);
    }

    void tab_view::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        const float header = t.tab_height;

        p.fill({b.x, b.y, b.width, header}, t.panel_header);
        p.fill({b.x, b.y + header - 1.0f, b.width, 1.0f}, t.separator);

        for (std::size_t i = 0; i < m_titles.size(); ++i)
        {
            const rect r = tab_rect(i);
            const bool on = static_cast<int>(i) == m_selected;
            const bool hot = static_cast<int>(i) == m_hot;
            if (on)
            {
                p.fill({r.x, r.y, r.width, r.height + 1.0f}, t.panel, t.radius);
                p.fill({r.x, r.bottom() - 2.0f, r.width, 2.0f}, t.panel);
                p.fill({r.x + 6.0f, r.y, r.width - 12.0f, 2.0f}, t.accent, 1.0f);
            }
            else if (hot)
            {
                p.fill(r, t.row_hover, t.radius);
            }
            p.text(r, m_titles[i], t.body_style(), on ? t.text : t.text_dim, align::center);
        }

        p.fill({b.x, b.y + header, b.width, b.height - header}, t.panel);
        clip_scope clip(p, {b.x, b.y + header, b.width, b.height - header});
        paint_children(p);
    }

    bool tab_view::on_pointer_down(const pointer_event &e)
    {
        if (e.button != mouse_button::left)
            return false;
        const int index = tab_at(e.position);
        if (index < 0)
            return false;
        choose(index);
        return true;
    }

    void tab_view::on_pointer_move(const pointer_event &e)
    {
        const int hot = tab_at(e.position);
        if (hot != m_hot)
        {
            m_hot = hot;
            invalidate();
        }
    }

    void tab_view::on_pointer_leave()
    {
        if (m_hot != -1)
            invalidate();
        m_hot = -1;
    }

    bool tab_view::on_key(const key_event &e)
    {
        if (e.action == action::release)
            return false;
        const modifiers held = e.mods & held_modifiers;
        if (held == modifiers::control && e.key == key::page_up)
        {
            choose(std::max(0, m_selected - 1));
            return true;
        }
        if (held == modifiers::control && e.key == key::page_down)
        {
            choose(std::min(static_cast<int>(m_titles.size()) - 1, m_selected + 1));
            return true;
        }
        // Ctrl+Tab cycles pages from anywhere inside.
        if (held == modifiers::control && e.key == key::tab && !m_titles.empty())
        {
            choose((m_selected + 1) % static_cast<int>(m_titles.size()));
            return true;
        }
        return false;
    }

} // namespace volt::ui
