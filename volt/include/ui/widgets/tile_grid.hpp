#pragma once

#include <ui/widget.hpp>

#include <cstddef>
#include <vector>

// -------------------------------------------------------------------------
// tile_grid
// -------------------------------------------------------------------------
//
// Children as tiles of equal width, as many to a row as fit at a minimum
// width, the rest wrapping onto further rows: cards that stand side by
// side in a wide window and stack in a narrow one, never squeezed below a
// width they can be read at (unless the grid itself is narrower than one
// tile, when the one column takes what there is).
//
// The tiles of a row share its height, the tallest one's, so that cards
// side by side line up top and bottom. Each tile is measured at the width
// it gets, so wrapping text inside one is as tall as it needs.

namespace volt::ui
{

    class tile_grid : public widget
    {
    public:
        explicit tile_grid(float min_tile_width = 180.0f, float spacing = 8.0f);

        void set_min_tile_width(float width);
        void set_spacing(float spacing);

        // How many columns a width makes room for, given `tiles` tiles: at
        // least one, at most `tiles`.
        int columns_for(float width, std::size_t tiles) const noexcept;

        // As laid out.
        int columns() const noexcept { return m_columns; }

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_arrange(const rect &bounds) override;

    private:
        // Measures the visible children at the tile width for `width`;
        // returns the grid's height and remembers each row's.
        float measure_rows(float width);

        float m_min_tile = 180.0f;
        float m_spacing = 8.0f;
        int m_columns = 1;

        // As last measured: the width the rows were measured at, and their
        // heights.
        float m_measured_width = -1.0f;
        std::vector<float> m_row_heights;
    };

} // namespace volt::ui
