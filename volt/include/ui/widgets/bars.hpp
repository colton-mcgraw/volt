#pragma once

#include <ui/layout/box.hpp>
#include <ui/painter.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/label.hpp>

#include <string>

// -------------------------------------------------------------------------
// toolbar, status_bar
// -------------------------------------------------------------------------
//
// The two bars of an application window. A toolbar is a row of flat tool
// buttons, usually icons with tooltips, in groups split by separators,
// along the top. A status bar is a row of short texts along the bottom, the
// last ones pushed to the right by a spacer; each segment is a label to set
// from outside.

namespace volt::ui
{

    class toolbar : public row
    {
    public:
        toolbar();

        // A flat button with an icon and a tooltip.
        button &add_tool(ui::icon icon, std::string tooltip, std::string text = {});

        void add_separator();
        void add_spacer();

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;
    };

    class status_bar : public row
    {
    public:
        status_bar();

        // A segment of text; `width`, if set, keeps it from changing size as
        // its text does.
        label &add_segment(std::string text = {}, float width = -1.0f);

        void add_separator();
        void add_spacer();

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;
    };

} // namespace volt::ui
