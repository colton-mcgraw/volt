#pragma once

#include <ui/signal.hpp>
#include <ui/widget.hpp>
#include <ui/widgets/scroll_area.hpp>

#include <cstddef>
#include <utility>

// -------------------------------------------------------------------------
// row_view
// -------------------------------------------------------------------------
//
// What list and tree views share: rows of one height, scrolled, with one
// selected. Rows are not widgets. A view of ten thousand rows lays out and
// paints only those that are visible: it asks its subclass to paint row i
// in a rectangle, for i in visible_range(), and nothing else, so its cost
// is that of a screenful whatever the row count.
//
// Pointer: a press selects the row under it; a double click, or Enter,
// emits `activated`. Keys: Up/Down, Page Up/Down, Home/End move the
// selection, scrolling it into view. The wheel scrolls.

namespace volt::ui
{

    class row_view : public widget
    {
    public:
        row_view();

        // How many rows there are.
        virtual std::size_t row_count() const = 0;

        int selected() const noexcept { return m_selected; }

        // Selects row `index` (-1 for none) and scrolls it into view; does
        // not emit.
        void set_selected(int index);

        // The theme's row height; a view of taller rows (two lines of text)
        // overrides it.
        virtual float row_height() const;

        // The rows that are at least partly visible: [first, last).
        std::pair<std::size_t, std::size_t> visible_range() const;

        // How many rows the last paint drew.
        std::size_t painted_rows() const noexcept { return m_painted; }

        float scroll_offset() const noexcept { return m_bar.offset(); }
        void scroll_to(float offset);
        // Scrolls row `index` into view. Before the view is shown and laid
        // out (it has no theme, so no row height, and no size), it is
        // remembered and done at the next arrange.
        void ensure_visible(int index);

        // The rectangle of row `index`, as laid out and scrolled now.
        rect row_rect(std::size_t index) const;

        // The row at `point`, or -1.
        int row_at(const math::vec2f &point) const;

        // Space at the top, inside the frame, that the rows and the
        // scrollbar leave free: where a table draws its header.
        void set_top_margin(float margin);
        float top_margin() const noexcept { return m_top_margin; }

        signal<int> selection_changed;
        signal<int> activated;

    protected:
        // Paints one row into `area`, the background (selection, hover)
        // already done.
        virtual void paint_row(painter &p, std::size_t index, const rect &area) = 0;

        // A press on a row, before it is selected: return true to stop it
        // (a tree's disclosure arrow).
        virtual bool row_pressed(std::size_t, const pointer_event &) { return false; }

        // Keys the subclass handles first (a tree's Left and Right).
        virtual bool row_key(const key_event &) { return false; }

        // Call when rows come or go.
        void rows_changed();

        // Selects as the user does: scrolls into view and emits.
        void choose(int index);

        math::vec2f on_measure(const constraints &limits) override;
        void on_arrange(const rect &bounds) override;
        void on_paint(painter &p) override;
        bool on_scroll(const scroll_input &e) override;
        bool on_pointer_down(const pointer_event &e) override;
        void on_pointer_move(const pointer_event &e) override;
        bool on_pointer_up(const pointer_event &e) override;
        void on_pointer_leave() override;
        bool on_click(const pointer_event &e) override;
        bool on_key(const key_event &e) override;
        void on_focus_changed(bool, focus_reason) override { invalidate(); }

    private:
        rect bar_rect() const;
        rect list_rect() const;

        scrollbar m_bar;
        int m_selected = -1;
        int m_hot = -1;
        bool m_bar_hot = false;
        std::size_t m_painted = 0;
        float m_top_margin = 0.0f;
        int m_pending_visible = -1; // ensure_visible() asked for before a layout
    };

} // namespace volt::ui
