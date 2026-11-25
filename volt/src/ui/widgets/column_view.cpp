#include <ui/widgets/column_view.hpp>

#include <ui/painter.hpp>
#include <ui/screen.hpp>
#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{
    namespace
    {
        // How close to a divider the pointer grabs it, either side.
        constexpr float divider_grab = 4.0f;

        // The narrowest a column can be dragged to.
        constexpr float min_column = 24.0f;

        // The sort arrow's size, and the room it takes after a title.
        constexpr float arrow_size = 10.0f;
    } // namespace

    column_view::column_view() = default;

    void column_view::set_columns(std::vector<table_column> columns)
    {
        m_columns = std::move(columns);
        m_x.assign(m_columns.size(), 0.0f);
        m_widths.assign(m_columns.size(), 0.0f);
        if (m_sort_column >= static_cast<int>(m_columns.size()))
            m_sort_column = -1;
        m_hot = -1;
        m_sizing = -1;
        m_pressed = -1;
        invalidate_layout();
    }

    void column_view::set_rows(std::size_t count, cell_text text)
    {
        m_rows = count;
        m_text = std::move(text);
        rows_changed();
    }

    void column_view::set_empty_text(std::string text)
    {
        if (text == m_empty_text)
            return;
        m_empty_text = std::move(text);
        invalidate();
    }

    void column_view::set_sort(int column, bool ascending)
    {
        if (column < -1 || column >= static_cast<int>(m_columns.size()))
            column = -1;
        if (column == m_sort_column && ascending == m_sort_ascending)
            return;
        m_sort_column = column;
        m_sort_ascending = ascending;
        invalidate();
    }

    // ---- geometry ---------------------------------------------------------------------

    float column_view::header_height() const
    {
        return theme().row_height + 4.0f;
    }

    rect column_view::header_rect() const
    {
        const rect inner = bounds().inset(1.0f);
        return {inner.x, inner.y, inner.width, std::min(inner.height, top_margin())};
    }

    float column_view::column_x(std::size_t index) const
    {
        return index < m_x.size() ? m_x[index] : 0.0f;
    }

    float column_view::column_width(std::size_t index) const
    {
        return index < m_widths.size() ? m_widths[index] : 0.0f;
    }

    void column_view::lay_out_columns()
    {
        const std::size_t n = m_columns.size();
        m_x.assign(n, 0.0f);
        m_widths.assign(n, 0.0f);
        if (n == 0)
            return;

        // The rows' width: the header lines up with them, scrollbar or not.
        const rect rows = row_rect(0);
        const float available = std::max(0.0f, rows.width);

        // Left out, highest drop_order first, while the widths do not fit.
        std::vector<bool> shown(n, true);
        const auto width_of_shown = [&] {
            float sum = 0.0f;
            for (std::size_t i = 0; i < n; ++i)
                sum += shown[i] ? std::max(0.0f, m_columns[i].width) : 0.0f;
            return sum;
        };
        while (width_of_shown() > available)
        {
            int drop = -1;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (shown[i] && m_columns[i].drop_order > 0 &&
                    (drop < 0 || m_columns[i].drop_order >= m_columns[static_cast<std::size_t>(drop)].drop_order))
                    drop = static_cast<int>(i);
            }
            if (drop < 0)
                break;
            shown[static_cast<std::size_t>(drop)] = false;
        }

        float wanted = 0.0f;
        float flex = 0.0f;
        std::size_t last_shown = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (!shown[i])
                continue;
            wanted += std::max(0.0f, m_columns[i].width);
            flex += std::max(0.0f, m_columns[i].flex);
            last_shown = i;
        }

        if (wanted >= available || wanted <= 0.0f)
        {
            // Still not room for every width: all shrink alike.
            const float factor = wanted > 0.0f ? available / wanted : 0.0f;
            for (std::size_t i = 0; i < n; ++i)
                m_widths[i] = shown[i] ? std::max(0.0f, m_columns[i].width) * factor : 0.0f;
        }
        else
        {
            // Room to spare: it goes by flex, or to the last column when no
            // column flexes (every one was sized by hand).
            const float extra = available - wanted;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (!shown[i])
                    continue;
                m_widths[i] = std::max(0.0f, m_columns[i].width);
                if (flex > 0.0f)
                    m_widths[i] += extra * std::max(0.0f, m_columns[i].flex) / flex;
            }
            if (flex <= 0.0f)
                m_widths[last_shown] += extra;
        }

        float x = rows.x;
        for (std::size_t i = 0; i < n; ++i)
        {
            m_x[i] = x;
            x += m_widths[i];
        }
    }

    int column_view::column_at(const math::vec2f &point) const
    {
        if (!header_rect().contains(point))
            return -1;
        for (std::size_t i = 0; i < m_columns.size(); ++i)
        {
            if (point.x() >= m_x[i] && point.x() < m_x[i] + m_widths[i])
                return static_cast<int>(i);
        }
        return -1;
    }

    int column_view::divider_at(const math::vec2f &point) const
    {
        if (!header_rect().contains(point))
            return -1;
        // The last column's right edge is the view's: nothing to drag there.
        for (std::size_t i = 0; i + 1 < m_columns.size(); ++i)
        {
            if (m_widths[i] <= 0.0f)
                continue;
            const float edge = m_x[i] + m_widths[i];
            if (std::abs(point.x() - edge) <= divider_grab)
                return static_cast<int>(i);
        }
        return -1;
    }

    cursor_shape column_view::cursor(const math::vec2f &point) const
    {
        if (m_sizing >= 0 || divider_at(point) >= 0)
            return cursor_shape::resize_horizontal;
        return row_view::cursor(point);
    }

    // ---- layout and painting -----------------------------------------------------------

    void column_view::on_attached()
    {
        set_top_margin(header_height());
    }

    void column_view::on_arrange(const rect &bounds)
    {
        row_view::on_arrange(bounds);
        lay_out_columns();
    }

    void column_view::on_paint(painter &p)
    {
        // The frame and the rows.
        row_view::on_paint(p);

        const ui::theme &t = theme();
        const bool enabled = is_enabled();

        if (m_rows == 0 && !m_empty_text.empty())
        {
            // Below the header, in the middle, wrapped to the width.
            const rect area = bounds().inset(insets{t.padding * 2.0f, top_margin() + t.padding * 2.0f,
                                                    t.padding * 2.0f, t.padding * 2.0f});
            if (!area.empty())
            {
                font::layout_options options;
                options.max_width = area.width;
                options.align = font::text_align::center;
                const font::text_layout lines = fonts().layout(m_empty_text, t.body_style(), options);
                const float top = area.y + std::max(0.0f, (area.height - lines.extent.y()) * 0.35f);
                clip_scope clip(p, area);
                p.layout(lines, t.body_style(), {area.x, top}, t.text_dim);
            }
        }

        // The header: a strip inside the frame, above the rows.
        const rect header = header_rect();
        if (header.empty())
            return;
        clip_scope clip(p, header);
        p.fill(header, t.panel_header, 0.0f);
        p.fill({header.x, header.bottom() - 1.0f, header.width, 1.0f}, t.separator);

        for (std::size_t i = 0; i < m_columns.size(); ++i)
        {
            const table_column &c = m_columns[i];
            const rect cell{m_x[i], header.y, m_widths[i], header.height - 1.0f};
            if (cell.width <= 0.0f)
                continue;
            if (static_cast<int>(i) == m_hot && c.sortable && enabled)
                p.fill(cell, t.row_hover);
            if (i + 1 < m_columns.size())
                p.fill({cell.right() - 1.0f, cell.y + 4.0f, 1.0f, cell.height - 8.0f}, t.separator);

            const bool sorted = static_cast<int>(i) == m_sort_column;
            rect text = cell.inset(insets::symmetric(t.control_padding * 0.6f, 0.0f));
            if (sorted)
                text.width = std::max(0.0f, text.width - arrow_size - 4.0f);
            p.text(text, c.title, t.small_style(), enabled ? (sorted ? t.text : t.text_dim) : t.text_disabled,
                   c.alignment);
            if (sorted && text.width > 0.0f)
            {
                const rect arrow{text.right() + 2.0f, cell.y + (cell.height - arrow_size) * 0.5f, arrow_size,
                                 arrow_size};
                p.draw_icon(m_sort_ascending ? icon::chevron_up : icon::chevron_down, arrow,
                            enabled ? t.text : t.text_disabled);
            }
        }
    }

    void column_view::paint_row(painter &p, std::size_t index, const rect &area)
    {
        const ui::theme &t = theme();
        const math::vec4f color = is_enabled() ? t.text : t.text_disabled;
        for (std::size_t i = 0; i < m_columns.size(); ++i)
        {
            const rect cell{m_x[i], area.y, m_widths[i], area.height};
            if (cell.width <= 2.0f)
                continue;
            if (m_cell_painter && m_cell_painter(p, index, i, cell))
                continue;
            if (!m_text)
                continue;
            const std::string_view text = m_text(index, i);
            if (text.empty())
                continue;
            p.text(cell.inset(insets::symmetric(t.control_padding * 0.6f, 0.0f)), text, t.body_style(), color,
                   m_columns[i].alignment);
        }
    }

    // ---- the header's pointer -------------------------------------------------------------

    bool column_view::on_pointer_down(const pointer_event &e)
    {
        if (!header_rect().contains(e.position))
            return row_view::on_pointer_down(e);
        if (e.button != mouse_button::left)
            return true;

        if (const int divider = divider_at(e.position); divider >= 0)
        {
            m_sizing = divider;
            m_sizing_from = e.position.x();
            m_sizing_width = m_widths[static_cast<std::size_t>(divider)];
            return true;
        }
        m_pressed = column_at(e.position);
        return true;
    }

    void column_view::on_pointer_move(const pointer_event &e)
    {
        if (m_sizing >= 0)
        {
            table_column &c = m_columns[static_cast<std::size_t>(m_sizing)];
            c.width = std::max(min_column, m_sizing_width + e.position.x() - m_sizing_from);
            c.flex = 0.0f;
            lay_out_columns();
            invalidate();
            return;
        }
        const int hot = column_at(e.position);
        if (hot != m_hot)
        {
            m_hot = hot;
            invalidate();
        }
        row_view::on_pointer_move(e);
    }

    bool column_view::on_pointer_up(const pointer_event &e)
    {
        if (m_sizing >= 0)
        {
            m_sizing = -1;
            return true;
        }
        return row_view::on_pointer_up(e);
    }

    void column_view::on_pointer_leave()
    {
        if (m_hot != -1)
            invalidate();
        m_hot = -1;
        row_view::on_pointer_leave();
    }

    bool column_view::on_click(const pointer_event &e)
    {
        if (!header_rect().contains(e.position))
            return row_view::on_click(e);

        const int column = column_at(e.position);
        const int pressed = m_pressed;
        m_pressed = -1;
        if (e.button != mouse_button::left || column < 0 || column != pressed ||
            !m_columns[static_cast<std::size_t>(column)].sortable)
            return true;

        const bool ascending = column == m_sort_column ? !m_sort_ascending : true;
        sort_requested.emit(column, ascending);
        return true;
    }

} // namespace volt::ui
