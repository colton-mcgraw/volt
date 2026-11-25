#include <ui/widgets/toggle_list.hpp>

#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{

    toggle_list::toggle_list(std::vector<toggle_row> rows) : m_rows(std::move(rows))
    {
    }

    void toggle_list::set_rows(std::vector<toggle_row> rows)
    {
        m_rows = std::move(rows);
        rows_changed();

        // A heading or a note may now be where the selection was.
        const int was = selected();
        if (was >= 0 && !selectable(static_cast<std::size_t>(was)))
        {
            int moved = nearest(was, 1);
            if (moved < 0)
                moved = nearest(was, -1);
            set_selected(moved);
        }
        invalidate();
    }

    bool toggle_list::selectable(std::size_t index) const noexcept
    {
        return index < m_rows.size() && !m_rows[index].header && m_rows[index].selectable;
    }

    void toggle_list::set_on(std::size_t index, bool on)
    {
        if (index >= m_rows.size() || m_rows[index].on == on)
            return;
        m_rows[index].on = on;
        invalidate();
    }

    void toggle_list::set_switch_icons(ui::icon on, ui::icon off)
    {
        m_on_icon = on;
        m_off_icon = off;
        invalidate();
    }

    rect toggle_list::switch_rect(std::size_t index) const
    {
        const rect r = row_rect(index);
        return {r.x + 2.0f, r.y, r.height + 2.0f, r.height};
    }

    int toggle_list::nearest(int from, int step) const
    {
        const int count = static_cast<int>(m_rows.size());
        for (int i = std::clamp(from, 0, std::max(0, count - 1)); i >= 0 && i < count; i += step)
        {
            if (selectable(static_cast<std::size_t>(i)))
                return i;
        }
        return -1;
    }

    void toggle_list::flip(std::size_t index)
    {
        if (index >= m_rows.size() || !m_rows[index].switchable || !is_enabled())
            return;
        toggled.emit(static_cast<int>(index), !m_rows[index].on);
    }

    void toggle_list::paint_row(painter &p, std::size_t index, const rect &area)
    {
        const ui::theme &t = theme();
        const toggle_row &r = m_rows[index];
        const bool enabled = is_enabled();
        const bool dim = r.dimmed || !enabled;

        // Headings: a faint band, so that the groups read as groups.
        if (r.header)
            p.fill(area.inset(insets::symmetric(2.0f, 1.0f)), faded(t.panel_header, 0.8f), t.small_radius);

        const rect sw = switch_rect(index);
        if (r.switchable)
        {
            const float size = std::min(t.icon_size, sw.height - 4.0f);
            const math::vec4f ink = !enabled ? t.text_disabled : r.on ? t.text_dim : faded(t.text_dim, 0.55f);
            p.draw_icon(r.on ? m_on_icon : m_off_icon,
                        {sw.x + (sw.width - size) * 0.5f, sw.y + (sw.height - size) * 0.5f, size, size}, ink);
        }

        float x = sw.right() + 2.0f;
        if (r.icon != icon::none)
        {
            const float size = 14.0f;
            p.draw_icon(r.icon, {x, area.y + (area.height - size) * 0.5f, size, size},
                        dim ? t.text_disabled : t.text_dim);
            x += size + t.spacing;
        }

        // The detail keeps its width; the text gets what is left.
        const text_style style = r.header ? t.small_style() : t.body_style();
        const text_style detail_style = t.small_style();
        float detail_width = 0.0f;
        if (!r.detail.empty())
            detail_width = std::min(std::ceil(fonts().measure(r.detail, detail_style)),
                                    std::max(0.0f, (area.right() - x) * 0.45f));
        const float right = area.right() - t.spacing;
        if (detail_width > 0.0f)
            p.text({right - detail_width, area.y, detail_width, area.height}, r.detail, detail_style,
                   dim ? t.text_disabled : t.text_dim, align::end);

        const float text_right = right - (detail_width > 0.0f ? detail_width + t.spacing : 0.0f);
        const math::vec4f ink = dim                           ? t.text_disabled
                                : r.header || !r.selectable   ? t.text_dim
                                                              : t.text;
        p.text({x, area.y, std::max(0.0f, text_right - x), area.height}, r.text, style, ink);
    }

    bool toggle_list::row_pressed(std::size_t index, const pointer_event &e)
    {
        if (e.button == mouse_button::left && m_rows[index].switchable && switch_rect(index).contains(e.position))
        {
            flip(index);
            return true;
        }
        // Headings and notes are not selected; a press on them does nothing
        // else either.
        return !selectable(index);
    }

    bool toggle_list::row_key(const key_event &e)
    {
        if ((e.mods & held_modifiers) != modifiers::none)
            return false;
        const int count = static_cast<int>(m_rows.size());
        if (count == 0)
            return false;
        const int at = selected();
        const int page = std::max(1, static_cast<int>(bounds().height / row_height()) - 1);

        // Moves to the selectable row nearest `target` going `step`, or the
        // other way when there is none that way; stays put with none at all.
        const auto go = [&](int target, int step) {
            int to = nearest(target, step);
            if (to < 0)
                to = nearest(target, -step);
            if (to >= 0)
                choose(to);
            return true;
        };

        switch (e.key)
        {
        case key::down:
            return go(at < 0 ? 0 : std::min(count - 1, at + 1), 1);
        case key::up:
            return go(at < 0 ? count - 1 : std::max(0, at - 1), -1);
        case key::page_down:
            return go(std::min(count - 1, std::max(0, at) + page), 1);
        case key::page_up:
            return go(std::max(0, at - page), -1);
        case key::home:
            return go(0, 1);
        case key::end:
            return go(count - 1, -1);
        case key::space:
            if (at < 0)
                return false;
            flip(static_cast<std::size_t>(at));
            return true;
        case key::del:
            if (at < 0)
                return false;
            delete_requested.emit(at);
            return true;
        default:
            return false;
        }
    }

} // namespace volt::ui
