#include <ui/layout/grid.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{

    void grid::solve_columns(float width)
    {
        const std::size_t count = m_columns.size();
        m_widths.assign(count, 0.0f);

        // Automatic columns are as wide as what sits in them alone.
        for (const auto &child : children())
        {
            const layout_params &p = child->layout();
            if (!child->visible() || p.column_span != 1 || p.column < 0 ||
                static_cast<std::size_t>(p.column) >= count)
                continue;
            const auto column = static_cast<std::size_t>(p.column);
            if (m_columns[column].kind == grid_track::sizing::automatic)
                m_widths[column] = std::max(m_widths[column], child->measured().x());
        }

        float used = m_column_spacing * static_cast<float>(count > 0 ? count - 1 : 0);
        float weights = 0.0f;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (m_columns[i].kind == grid_track::sizing::fixed)
                m_widths[i] = m_columns[i].value;
            if (m_columns[i].kind == grid_track::sizing::flex)
                weights += m_columns[i].value;
            else
                used += m_widths[i];
        }

        const float free = std::isfinite(width) ? std::max(0.0f, width - used) : 0.0f;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (m_columns[i].kind == grid_track::sizing::flex && weights > 0.0f)
                m_widths[i] = std::floor(free * m_columns[i].value / weights);
        }
    }

    math::vec2f grid::on_measure(const constraints &limits)
    {
        // Measure loose first, for the automatic columns.
        int rows = 0;
        for (const auto &child : children())
        {
            if (!child->visible())
                continue;
            child->measure(constraints::loose({limits.max.x(), unbounded}));
            rows = std::max(rows, child->layout().row + 1);
        }
        solve_columns(limits.max.x());

        // Then each in the width it will have, for the row heights (text
        // that wraps grows taller in a narrow column).
        m_heights.assign(static_cast<std::size_t>(rows), 0.0f);
        for (const auto &child : children())
        {
            const layout_params &p = child->layout();
            if (!child->visible() || p.row < 0 || p.column < 0)
                continue;
            float width = 0.0f;
            for (int c = p.column; c < p.column + p.column_span && static_cast<std::size_t>(c) < m_widths.size(); ++c)
                width += m_widths[static_cast<std::size_t>(c)] + (c > p.column ? m_column_spacing : 0.0f);
            const math::vec2f size = child->measure(constraints::loose({width, unbounded}));
            float &height = m_heights[static_cast<std::size_t>(p.row)];
            height = std::max(height, size.y());
        }

        float width = m_column_spacing * static_cast<float>(m_widths.empty() ? 0 : m_widths.size() - 1);
        for (float w : m_widths)
            width += w;
        float height = m_row_spacing * static_cast<float>(rows > 0 ? rows - 1 : 0);
        for (float h : m_heights)
            height += h;
        return {width, height};
    }

    void grid::on_arrange(const rect &bounds)
    {
        solve_columns(bounds.width);

        std::vector<float> lefts(m_widths.size(), bounds.x);
        for (std::size_t i = 1; i < m_widths.size(); ++i)
            lefts[i] = lefts[i - 1] + m_widths[i - 1] + m_column_spacing;
        std::vector<float> tops(m_heights.size(), bounds.y);
        for (std::size_t i = 1; i < m_heights.size(); ++i)
            tops[i] = tops[i - 1] + m_heights[i - 1] + m_row_spacing;

        for (const auto &child : children())
        {
            const layout_params &p = child->layout();
            if (!child->visible() || p.row < 0 || p.column < 0 || static_cast<std::size_t>(p.row) >= tops.size() ||
                static_cast<std::size_t>(p.column) >= lefts.size())
            {
                child->arrange({bounds.x, bounds.y, 0.0f, 0.0f});
                continue;
            }

            const auto column = static_cast<std::size_t>(p.column);
            const auto row = static_cast<std::size_t>(p.row);
            float width = 0.0f;
            const std::size_t end = std::min(column + static_cast<std::size_t>(p.column_span), m_widths.size());
            for (std::size_t c = column; c < end; ++c)
                width += m_widths[c] + (c > column ? m_column_spacing : 0.0f);

            const float row_height = m_heights[row];
            const align across = p.cross.value_or(align::stretch);
            float height = across == align::stretch ? row_height : std::min(child->measured().y(), row_height);
            float top = tops[row];
            if (across == align::center)
                top += std::floor((row_height - height) * 0.5f);
            else if (across == align::end)
                top += row_height - height;

            child->arrange({lefts[column], top, width, height});
        }
    }

} // namespace volt::ui
