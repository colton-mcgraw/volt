#pragma once

#include <ui/painter.hpp>
#include <ui/signal.hpp>
#include <ui/widgets/row_view.hpp>

#include <cstddef>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// toggle_list
// -------------------------------------------------------------------------
//
// A row view (<ui/widgets/row_view.hpp>) of things that can each be switched
// on and off, in groups: the objects on a board with an eye to show or hide
// each, layers with theirs. A row is, left to right, its switch (an eye by
// default), an icon, its text, and a short detail in the dim colour at the
// right. Group headings are rows too: they may carry a switch (the whole
// group's), but are never selected; nor are note rows ("Nothing here yet").
//
// Pointer: a press on a switch flips it and emits `toggled`, without
// selecting the row; a press elsewhere on a row selects it. Keys: those of
// every row view, moving over headings and notes as if they were not there;
// Space flips the selected row's switch; Delete asks for the selected row
// to go (`delete_requested`), which the owner may or may not do.
//
// The rows are the owner's to keep up to date: the list never flips a
// switch by itself (the owner calls set_on(), or set_rows() again, after
// acting on `toggled`), so what it shows is what is true.

namespace volt::ui
{

    struct toggle_row
    {
        std::string text;
        std::string detail; // dim, at the right
        ui::icon icon = icon::none;

        bool header = false;     // a group's heading
        bool switchable = true;  // has a switch
        bool on = true;          // the switch
        bool dimmed = false;     // drawn greyed: switched off by its group, say
        bool selectable = true;  // false: a note; headings never are
    };

    class toggle_list : public row_view
    {
    public:
        explicit toggle_list(std::vector<toggle_row> rows = {});

        // Replaces the rows. The selected index stays where it is (moved to
        // a row that can be selected, or none).
        void set_rows(std::vector<toggle_row> rows);
        const std::vector<toggle_row> &rows() const noexcept { return m_rows; }
        const toggle_row &row(std::size_t index) const { return m_rows[index]; }

        std::size_t row_count() const override { return m_rows.size(); }

        // Whether row `index` can be selected.
        bool selectable(std::size_t index) const noexcept;

        // Sets a row's switch; does not emit.
        void set_on(std::size_t index, bool on);

        // The icons the switch shows on and off (an eye open and shut by
        // default).
        void set_switch_icons(ui::icon on, ui::icon off);

        // Where row `index`'s switch is, as laid out and scrolled now.
        rect switch_rect(std::size_t index) const;

        // The user flipped row `index`'s switch; the bool is what they asked
        // for.
        signal<int, bool> toggled;

        // Delete was pressed with row `index` selected.
        signal<int> delete_requested;

    protected:
        void paint_row(painter &p, std::size_t index, const rect &area) override;
        bool row_pressed(std::size_t index, const pointer_event &e) override;
        bool row_key(const key_event &e) override;

    private:
        // The selectable row nearest to `from` going `step` (+1 or -1)
        // rows at a time, `from` included; -1 if none.
        int nearest(int from, int step) const;
        void flip(std::size_t index);

        std::vector<toggle_row> m_rows;
        ui::icon m_on_icon = icon::eye;
        ui::icon m_off_icon = icon::eye_off;
    };

} // namespace volt::ui
