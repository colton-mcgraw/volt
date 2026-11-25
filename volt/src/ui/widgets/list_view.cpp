#include <ui/widgets/list_view.hpp>

#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{

    // ---- list_view ----------------------------------------------------------------

    list_view::list_view(std::vector<std::string> items) : m_items(std::move(items))
    {
    }

    void list_view::set_items(std::vector<std::string> items)
    {
        m_items = std::move(items);
        rows_changed();
    }

    void list_view::paint_row(painter &p, std::size_t index, const rect &area)
    {
        const ui::theme &t = theme();
        p.text(area.inset(insets::symmetric(8.0f, 0.0f)), m_items[index], t.body_style(),
               is_enabled() ? t.text : t.text_disabled);
    }

    // ---- tree_view ----------------------------------------------------------------

    tree_view::tree_view(std::vector<tree_node> roots) : m_roots(std::move(roots))
    {
        rebuild();
    }

    void tree_view::set_roots(std::vector<tree_node> roots)
    {
        m_roots = std::move(roots);
        rebuild();
        rows_changed();
    }

    void tree_view::rebuild()
    {
        // Keep the selection on the same node if it is still shown.
        const tree_node *was = selected_node();

        m_rows.clear();
        const auto visit = [&](auto &&self, std::vector<tree_node> &nodes, int depth, int parent) -> void {
            for (tree_node &n : nodes)
            {
                const int index = static_cast<int>(m_rows.size());
                m_rows.push_back({&n, depth, parent});
                if (n.expanded)
                    self(self, n.children, depth + 1, index);
            }
        };
        visit(visit, m_roots, 0, -1);

        if (was != nullptr)
        {
            for (std::size_t i = 0; i < m_rows.size(); ++i)
            {
                if (m_rows[i].node == was)
                {
                    set_selected(static_cast<int>(i));
                    return;
                }
            }
        }
    }

    tree_node *tree_view::selected_node() const
    {
        const int index = selected();
        if (index < 0 || static_cast<std::size_t>(index) >= m_rows.size())
            return nullptr;
        return m_rows[static_cast<std::size_t>(index)].node;
    }

    void tree_view::set_expanded(std::size_t index, bool expanded)
    {
        if (index >= m_rows.size())
            return;
        tree_node &n = *m_rows[index].node;
        if (n.children.empty() || n.expanded == expanded)
            return;
        n.expanded = expanded;
        rebuild();
        rows_changed();
    }

    void tree_view::toggle(std::size_t index)
    {
        if (index < m_rows.size())
            set_expanded(index, !m_rows[index].node->expanded);
    }

    void tree_view::expand_all()
    {
        const auto open = [](auto &&self, std::vector<tree_node> &nodes) -> void {
            for (tree_node &n : nodes)
            {
                n.expanded = !n.children.empty();
                self(self, n.children);
            }
        };
        open(open, m_roots);
        rebuild();
        rows_changed();
    }

    rect tree_view::arrow_rect(std::size_t index) const
    {
        const rect r = row_rect(index);
        const float indent = theme().indent;
        return {r.x + 4.0f + indent * static_cast<float>(m_rows[index].depth), r.y, indent, r.height};
    }

    void tree_view::paint_row(painter &p, std::size_t index, const rect &area)
    {
        const ui::theme &t = theme();
        const row &r = m_rows[index];
        const bool enabled = is_enabled();

        const rect arrow = arrow_rect(index);
        if (!r.node->children.empty())
            p.draw_icon(r.node->expanded ? icon::chevron_down : icon::chevron_right,
                        {arrow.x + 1.0f, arrow.y + (arrow.height - 12.0f) * 0.5f, 12.0f, 12.0f}, t.text_dim);

        float x = arrow.right();
        if (r.node->icon != icon::none)
        {
            const float size = 14.0f;
            p.draw_icon(r.node->icon, {x, area.y + (area.height - size) * 0.5f, size, size},
                        enabled ? t.text_dim : t.text_disabled);
            x += size + 6.0f;
        }
        p.text({x, area.y, std::max(0.0f, area.right() - x - 4.0f), area.height}, r.node->text, t.body_style(),
               enabled ? t.text : t.text_disabled);
    }

    bool tree_view::row_pressed(std::size_t index, const pointer_event &e)
    {
        if (e.button == mouse_button::left && !m_rows[index].node->children.empty() &&
            arrow_rect(index).contains(e.position))
        {
            toggle(index);
            return true;
        }
        return false;
    }

    bool tree_view::on_click(const pointer_event &e)
    {
        if (e.button == mouse_button::left && e.clicks == 2)
        {
            const int index = row_at(e.position);
            if (index >= 0 && !m_rows[static_cast<std::size_t>(index)].node->children.empty())
            {
                toggle(static_cast<std::size_t>(index));
                return true;
            }
        }
        return row_view::on_click(e);
    }

    bool tree_view::row_key(const key_event &e)
    {
        if ((e.mods & held_modifiers) != modifiers::none)
            return false;
        const int index = selected();
        if (index < 0)
            return false;
        const auto at = static_cast<std::size_t>(index);
        const row &r = m_rows[at];

        if (e.key == key::right)
        {
            if (!r.node->children.empty() && !r.node->expanded)
                set_expanded(at, true);
            else if (r.node->expanded)
                choose(index + 1);
            return true;
        }
        if (e.key == key::left)
        {
            if (r.node->expanded)
                set_expanded(at, false);
            else if (r.parent >= 0)
                choose(r.parent);
            return true;
        }
        return false;
    }

} // namespace volt::ui
