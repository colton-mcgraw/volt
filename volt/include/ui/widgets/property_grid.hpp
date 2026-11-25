#pragma once

#include <ui/widget.hpp>

#include <memory>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// property_grid
// -------------------------------------------------------------------------
//
// Name and value rows, the value being an editor widget of any kind (a
// field, a checkbox, a dropdown, a slider), grouped under section headers
// that fold away on a click. The names take a share of the width (with a
// minimum), the editors the rest. Every row is one height, unless its
// editor measures taller.
//
// The editors are real widgets, children of the grid, so they take the
// focus and Tab moves through them in row order.

namespace volt::ui
{

    class property_grid : public widget
    {
    public:
        property_grid();

        // Starts a section; the rows added after it belong to it.
        void add_section(std::string title, bool expanded = true);

        // A row with `editor` as its value; returns the editor.
        template <typename W>
        W &add_row(std::string name, std::unique_ptr<W> editor)
        {
            W &result = *editor;
            add_entry(std::move(name), std::move(editor));
            return result;
        }

        // The share of the width the names get, 0 to 1.
        void set_name_share(float share);

        std::size_t row_count() const noexcept { return m_entries.size(); }

        bool section_expanded(std::size_t section) const { return m_sections[section].expanded; }
        void set_section_expanded(std::size_t section, bool expanded);

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_arrange(const rect &bounds) override;
        void on_paint(painter &p) override;
        bool on_pointer_down(const pointer_event &e) override;

    private:
        struct section
        {
            std::string title;
            bool expanded = true;
            rect header; // as laid out
        };

        struct entry
        {
            std::string name;
            widget *editor = nullptr;
            int section = -1;
            rect area; // the whole row, as laid out
        };

        void add_entry(std::string name, std::unique_ptr<widget> editor);
        float name_width(float width) const;
        float row_height_for(const entry &e) const;

        std::vector<section> m_sections;
        std::vector<entry> m_entries;
        float m_share = 0.42f;
    };

} // namespace volt::ui
