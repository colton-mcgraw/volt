#include <ui/widgets/tile_grid.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace volt::ui
{

    tile_grid::tile_grid(float min_tile_width, float spacing)
        : m_min_tile(std::max(1.0f, min_tile_width)), m_spacing(std::max(0.0f, spacing))
    {
        set_hit_testable(false);
    }

    void tile_grid::set_min_tile_width(float width)
    {
        m_min_tile = std::max(1.0f, width);
        invalidate_layout();
    }

    void tile_grid::set_spacing(float spacing)
    {
        m_spacing = std::max(0.0f, spacing);
        invalidate_layout();
    }

    int tile_grid::columns_for(float width, std::size_t tiles) const noexcept
    {
        if (tiles == 0)
            return 1;
        const float fit = std::floor((width + m_spacing) / (m_min_tile + m_spacing));
        const int columns = std::isfinite(fit) ? static_cast<int>(std::max(fit, 1.0f)) : static_cast<int>(tiles);
        return std::clamp(columns, 1, static_cast<int>(tiles));
    }

    float tile_grid::measure_rows(float width)
    {
        std::vector<widget *> shown;
        for (const auto &child : children())
        {
            if (child->visible())
                shown.push_back(child.get());
        }

        m_columns = columns_for(width, shown.size());
        m_row_heights.clear();
        m_measured_width = width;
        if (shown.empty())
            return 0.0f;

        const float columns = static_cast<float>(m_columns);
        const float tile = std::max(0.0f, std::floor((width - m_spacing * (columns - 1.0f)) / columns));
        float height = 0.0f;
        for (std::size_t i = 0; i < shown.size(); ++i)
        {
            const math::vec2f size = shown[i]->measure({{tile, 0.0f}, {tile, unbounded}});
            if (i % static_cast<std::size_t>(m_columns) == 0)
                m_row_heights.push_back(0.0f);
            m_row_heights.back() = std::max(m_row_heights.back(), size.y());
        }
        for (float row : m_row_heights)
            height += row;
        height += m_spacing * static_cast<float>(m_row_heights.size() - 1);
        return height;
    }

    math::vec2f tile_grid::on_measure(const constraints &limits)
    {
        // Without a width to fit into, the tiles stand in one row at their
        // minimum width.
        std::size_t shown = 0;
        for (const auto &child : children())
            shown += child->visible() ? 1 : 0;
        float width = limits.max.x();
        if (!std::isfinite(width))
            width = shown == 0 ? 0.0f : m_min_tile * static_cast<float>(shown) + m_spacing * static_cast<float>(shown - 1);

        const float height = measure_rows(width);
        return {std::min(width, m_min_tile), height};
    }

    void tile_grid::on_arrange(const rect &bounds)
    {
        // Laid out at another width than measured (a parent that stretches
        // it): the tiles' heights depend on their width, so again.
        if (bounds.width != m_measured_width)
            measure_rows(bounds.width);

        const float columns = static_cast<float>(m_columns);
        const float tile = std::max(0.0f, std::floor((bounds.width - m_spacing * (columns - 1.0f)) / columns));
        float y = bounds.y;
        std::size_t i = 0;
        for (const auto &child : children())
        {
            if (!child->visible())
            {
                child->arrange({bounds.x, bounds.y, 0.0f, 0.0f});
                continue;
            }
            const std::size_t row = i / static_cast<std::size_t>(m_columns);
            const std::size_t column = i % static_cast<std::size_t>(m_columns);
            if (column == 0 && row > 0)
                y += m_row_heights[row - 1] + m_spacing;
            const float x = bounds.x + static_cast<float>(column) * (tile + m_spacing);
            child->arrange({x, y, tile, m_row_heights[row]});
            ++i;
        }
    }

} // namespace volt::ui
