#pragma once

#include <ui/widget.hpp>

#include <cstdint>
#include <vector>

// -------------------------------------------------------------------------
// grid
// -------------------------------------------------------------------------
//
// Children in rows and columns, each in the cell its layout params name
// (layout_params::row, column, column_span). Rows are as tall as their
// tallest child; columns are sized per their track:
//
//   fixed      that many units;
//   automatic  as wide as the widest child that sits in it alone;
//   flex       a share, by weight, of what the others leave.
//
// A child fills its cell across (spanned columns and the spacing between
// them included) and is centred in its row's height unless it stretches
// (layout_params::cross, align::stretch by default).

namespace volt::ui
{

    struct grid_track
    {
        enum class sizing : std::uint8_t
        {
            fixed,
            automatic,
            flex,
        };

        sizing kind = sizing::automatic;
        float value = 0.0f; // units for fixed, weight for flex

        static constexpr grid_track fixed(float units) noexcept { return {sizing::fixed, units}; }
        static constexpr grid_track automatic() noexcept { return {sizing::automatic, 0.0f}; }
        static constexpr grid_track flex(float weight = 1.0f) noexcept { return {sizing::flex, weight}; }
    };

    class grid : public widget
    {
    public:
        explicit grid(std::vector<grid_track> columns, float column_spacing = 0.0f, float row_spacing = 0.0f)
            : m_columns(std::move(columns)), m_column_spacing(column_spacing), m_row_spacing(row_spacing)
        {
        }

        // Places `child` in a cell and returns it.
        template <typename W>
        W &place(std::unique_ptr<W> child, int row, int column, int column_span = 1)
        {
            child->layout().row = row;
            child->layout().column = column;
            child->layout().column_span = column_span;
            return add(std::move(child));
        }

        std::size_t column_count() const noexcept { return m_columns.size(); }

        // The widths and the row heights of the last layout.
        const std::vector<float> &column_widths() const noexcept { return m_widths; }
        const std::vector<float> &row_heights() const noexcept { return m_heights; }

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_arrange(const rect &bounds) override;

    private:
        void solve_columns(float width);

        std::vector<grid_track> m_columns;
        float m_column_spacing;
        float m_row_spacing;

        std::vector<float> m_widths;
        std::vector<float> m_heights;
    };

} // namespace volt::ui
