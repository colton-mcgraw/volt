#pragma once

#include <ui/layout/box.hpp>
#include <ui/painter.hpp>
#include <ui/signal.hpp>
#include <ui/widgets/button.hpp>

#include <cstddef>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// tool_strip
// -------------------------------------------------------------------------
//
// A bar of tools of which exactly one is active: an editor's tool palette
// standing beside its canvas (vertical), or a row of choices such as the
// sheets of a schematic (horizontal). Each tool is a flat toggle button,
// an icon with a tooltip, or an icon and a name; the active one stays
// down. Clicking a tool makes it the active one and emits `changed`;
// clicking the active one again changes nothing.
//
// Like a toolbar's, the tools do not take the keyboard focus from the
// document beside them: the keyboard reaches them through the shortcuts
// the window binds (and shows in the tooltips) and through select().
//
// The strip paints a panel surface with a separator line along the edge
// that faces the document: the right edge of a vertical strip, the bottom
// of a horizontal one.

namespace volt::ui
{

    class tool_strip : public box
    {
    public:
        explicit tool_strip(ui::orientation orientation = orientation::vertical);

        // Adds a tool and returns its index. The first tool added is the
        // active one until another is chosen.
        int add_tool(ui::icon icon, std::string tooltip, std::string text = {});

        // A thin line between groups of tools.
        void add_separator();

        std::size_t tool_count() const noexcept { return m_tools.size(); }
        button &tool(std::size_t index) const { return *m_tools[index]; }

        // The active tool, -1 while there is none.
        int selected() const noexcept { return m_selected; }

        // Makes `index` the active tool; does not emit. Out of range is
        // ignored.
        void select(int index);

        // The user chose a tool, by its index.
        signal<int> changed;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;

    private:
        void choose(int index);
        void sync_checks();

        std::vector<button *> m_tools;
        int m_selected = -1;
    };

} // namespace volt::ui
