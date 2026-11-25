#include <ui/widgets/property_grid.hpp>

#include <ui/painter.hpp>
#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{
    namespace
    {
        constexpr float minimum_name = 70.0f;
        constexpr float row_padding = 3.0f;
    } // namespace

    property_grid::property_grid() = default;

    void property_grid::add_section(std::string title, bool expanded)
    {
        m_sections.push_back({std::move(title), expanded, {}});
        invalidate_layout();
    }

    void property_grid::add_entry(std::string name, std::unique_ptr<widget> editor)
    {
        const int section_index = static_cast<int>(m_sections.size()) - 1;
        widget &added = *editor;
        add(std::move(editor));
        m_entries.push_back({std::move(name), &added, section_index, {}});
        if (section_index >= 0 && !m_sections[static_cast<std::size_t>(section_index)].expanded)
            added.set_visible(false);
    }

    void property_grid::set_name_share(float share)
    {
        m_share = std::clamp(share, 0.1f, 0.9f);
        invalidate_layout();
    }

    void property_grid::set_section_expanded(std::size_t index, bool expanded)
    {
        if (index >= m_sections.size() || m_sections[index].expanded == expanded)
            return;
        m_sections[index].expanded = expanded;
        for (entry &e : m_entries)
        {
            if (e.section == static_cast<int>(index))
                e.editor->set_visible(expanded);
        }
        invalidate_layout();
    }

    float property_grid::name_width(float width) const
    {
        return std::floor(std::max(minimum_name, width * m_share));
    }

    float property_grid::row_height_for(const entry &e) const
    {
        return std::max(theme().control_height, e.editor->measured().y()) + 2.0f * row_padding;
    }

    math::vec2f property_grid::on_measure(const constraints &limits)
    {
        const ui::theme &t = theme();
        const float width = std::isfinite(limits.max.x()) ? limits.max.x() : 280.0f;
        const float value_width = std::max(0.0f, width - name_width(width) - t.padding);

        float height = 0.0f;
        int current = -2;
        for (entry &e : m_entries)
        {
            if (e.section != current)
            {
                current = e.section;
                if (current >= 0)
                    height += t.row_height + 4.0f;
            }
            if (!e.editor->visible())
                continue;
            e.editor->measure(constraints::loose({value_width, unbounded}));
            height += row_height_for(e);
        }
        // Sections with no rows still show their header.
        if (m_entries.empty())
            height += static_cast<float>(m_sections.size()) * (t.row_height + 4.0f);
        return {width, height};
    }

    void property_grid::on_arrange(const rect &bounds)
    {
        const ui::theme &t = theme();
        const float names = name_width(bounds.width);
        float y = bounds.y;
        int current = -2;

        for (section &s : m_sections)
            s.header = {};

        for (entry &e : m_entries)
        {
            if (e.section != current)
            {
                current = e.section;
                if (current >= 0)
                {
                    section &s = m_sections[static_cast<std::size_t>(current)];
                    s.header = {bounds.x, y, bounds.width, t.row_height + 4.0f};
                    y += s.header.height;
                }
            }
            if (!e.editor->visible())
            {
                e.area = {bounds.x, y, bounds.width, 0.0f};
                e.editor->arrange({bounds.x, y, 0.0f, 0.0f});
                continue;
            }

            const float height = row_height_for(e);
            e.area = {bounds.x, y, bounds.width, height};
            const float value_left = bounds.x + names;
            const float value_width = std::max(0.0f, bounds.right() - value_left - t.padding);
            const float editor_height = std::min(e.editor->measured().y(), height - 2.0f * row_padding);

            // Editors that want little width (a checkbox) keep it; the rest
            // fill the column.
            const bool narrow = e.editor->layout().cross == align::start;
            const float editor_width = narrow ? std::min(e.editor->measured().x(), value_width) : value_width;
            const float editor_top = std::round(y + (height - editor_height) * 0.5f);
            e.editor->arrange({value_left, editor_top, editor_width, editor_height});
            y += height;
        }
    }

    void property_grid::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const float names = name_width(bounds().width);

        for (const section &s : m_sections)
        {
            if (s.header.empty())
                continue;
            p.fill(s.header, t.panel_header);
            p.draw_icon(s.expanded ? icon::chevron_down : icon::chevron_right,
                        {s.header.x + 6.0f, s.header.y + (s.header.height - 12.0f) * 0.5f, 12.0f, 12.0f}, t.text_dim);
            p.text({s.header.x + 24.0f, s.header.y, s.header.width - 28.0f, s.header.height}, s.title,
                   t.small_style(), t.text_dim);
        }

        for (const entry &e : m_entries)
        {
            if (e.area.height <= 0.0f)
                continue;
            p.text({e.area.x + 24.0f, e.area.y, names - 28.0f, e.area.height}, e.name, t.body_style(),
                   e.editor->is_enabled() ? t.text_dim : t.text_disabled);
        }
        paint_children(p);
    }

    bool property_grid::on_pointer_down(const pointer_event &e)
    {
        if (e.button != mouse_button::left)
            return false;
        for (std::size_t i = 0; i < m_sections.size(); ++i)
        {
            if (m_sections[i].header.contains(e.position))
            {
                set_section_expanded(i, !m_sections[i].expanded);
                return true;
            }
        }
        return false;
    }

} // namespace volt::ui
