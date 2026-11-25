#pragma once

#include <ui/geometry.hpp>
#include <ui/signal.hpp>
#include <ui/widgets/row_view.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// column_view
// -------------------------------------------------------------------------
//
// A row view (<ui/widgets/row_view.hpp>) with several columns under a
// header: a table to read, not to edit. It holds no rows of its own: it is
// told how many there are and asks for the text of a cell only while
// painting it, so a hundred thousand rows cost what a screenful does, and
// the data stays where the application keeps it.
//
// Columns. Each has a width, which it gets at least while there is room,
// and a flex weight for its share of the space left over. Where there is
// not room for every width, the columns that may be dropped are left out
// (by their drop_order), and then the rest shrink in proportion, their
// text cut with an ellipsis. Dragging the divider at the right of a header cell
// sizes that column: it keeps the width it was given (its flex goes to 0)
// and the flexible columns take up the difference.
//
// Sorting is the owner's business: clicking a sortable column's header
// asks for it (sort_requested, with the order it asks for: ascending on a
// new column, the other way on the sorted one); the owner sorts its rows
// and says which column it sorted by with set_sort(), which only draws the
// arrow. No column marked is a valid state (search hits by relevance).
//
// Keys and pointer as in any row view: Up, Down, Page Up/Down, Home/End
// move the selection, Enter or a double click emits `activated`. With no
// rows the view says `empty_text` in its middle, for a list that would
// otherwise just be blank.

namespace volt::ui
{

    class painter;

    struct table_column
    {
        std::string title;
        // At least this wide while the view has room; logical units.
        float width = 100.0f;
        // Share of the width left over once every column has its width.
        float flex = 1.0f;
        align alignment = align::start;
        bool sortable = true;
        // Where there is not room for every column's width, the columns
        // with the highest number here are left out first, until the rest
        // fit; 0 is never left out.
        int drop_order = 0;
    };

    class column_view : public row_view
    {
    public:
        // The text of cell (row, column); called while painting, for the
        // visible rows only. What it returns must stay valid while the
        // row is painted.
        using cell_text = std::function<std::string_view(std::size_t row, std::size_t column)>;

        // Paints a cell itself instead (a status marker); returns false to
        // let the view paint its text as usual.
        using cell_painter = std::function<bool(painter &p, std::size_t row, std::size_t column, const rect &area)>;

        column_view();

        void set_columns(std::vector<table_column> columns);
        const std::vector<table_column> &columns() const noexcept { return m_columns; }
        std::size_t column_count() const noexcept { return m_columns.size(); }

        // The number of rows and where their text comes from. The selected
        // index is kept (clamped); the owner moves it with set_selected()
        // when the rows' order changed.
        void set_rows(std::size_t count, cell_text text);
        std::size_t row_count() const override { return m_rows; }

        void set_cell_painter(cell_painter paint) { m_cell_painter = std::move(paint); }

        // Said in the middle of the view while it has no rows.
        void set_empty_text(std::string text);
        const std::string &empty_text() const noexcept { return m_empty_text; }

        // ---- sorting -------------------------------------------------------

        // The arrow in the header: on `column` (-1 for none), pointing up
        // for ascending.
        void set_sort(int column, bool ascending);
        int sort_column() const noexcept { return m_sort_column; }
        bool sort_ascending() const noexcept { return m_sort_ascending; }

        // A sortable column's header was clicked: sort by it this way.
        signal<int, bool> sort_requested;

        // ---- geometry, as laid out ------------------------------------------

        float header_height() const;
        rect header_rect() const;

        // Column `index`'s left edge (absolute) and width; 0 wide while it
        // is left out for want of room.
        float column_x(std::size_t index) const;
        float column_width(std::size_t index) const;

        // The column whose header is at `point`, or -1.
        int column_at(const math::vec2f &point) const;

        cursor_shape cursor(const math::vec2f &point) const override;

    protected:
        void paint_row(painter &p, std::size_t index, const rect &area) override;

        void on_attached() override;
        void on_arrange(const rect &bounds) override;
        void on_paint(painter &p) override;
        bool on_pointer_down(const pointer_event &e) override;
        void on_pointer_move(const pointer_event &e) override;
        bool on_pointer_up(const pointer_event &e) override;
        void on_pointer_leave() override;
        bool on_click(const pointer_event &e) override;

    private:
        void lay_out_columns();
        // The column whose right-hand divider is at `point`, or -1.
        int divider_at(const math::vec2f &point) const;

        std::vector<table_column> m_columns;
        std::vector<float> m_x;      // as laid out, absolute
        std::vector<float> m_widths; // as laid out
        std::size_t m_rows = 0;
        cell_text m_text;
        cell_painter m_cell_painter;
        std::string m_empty_text;

        int m_sort_column = -1;
        bool m_sort_ascending = true;

        int m_hot = -1;           // header cell under the pointer
        int m_sizing = -1;        // column being sized by its divider
        float m_sizing_from = 0.0f;
        float m_sizing_width = 0.0f;
        int m_pressed = -1;       // header cell pressed, for the click
    };

} // namespace volt::ui
