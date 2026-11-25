#pragma once

#include <ui/layout/box.hpp>

#include <cstdint>
#include <string>

// -------------------------------------------------------------------------
// panel, separator
// -------------------------------------------------------------------------
//
// A panel is a column with a surface under it: a background from the theme,
// optionally a border, and optionally a title in a header strip above its
// children (a group box, a docked panel's caption). It lays its children out
// as a column does, below the header.
//
// A separator is a thin line across a box, between groups of things.

namespace volt::ui
{

    enum class surface : std::uint8_t
    {
        none,       // transparent
        background, // the window's
        panel,
        header,
        raised,
        field,
    };

    class panel : public box
    {
    public:
        explicit panel(std::string title = {}, ui::surface surface = surface::panel, float spacing = 6.0f,
                       insets padding = insets::uniform(8.0f));

        void set_title(std::string title);
        const std::string &title() const noexcept { return m_title; }

        void set_surface(ui::surface surface);
        void set_border(bool border);

        // Which edges get a separator line, for panels that sit against
        // others (a side panel's right edge, a bar's bottom).
        void set_edges(bool top, bool bottom, bool left, bool right);

        float header_height() const;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_arrange(const rect &bounds) override;
        void on_paint(painter &p) override;

    private:
        std::string m_title;
        ui::surface m_surface;
        bool m_border = false;
        bool m_edges[4] = {false, false, false, false};
    };

    class separator : public widget
    {
    public:
        // `along` is the direction of the line: horizontal separates the
        // items of a column, vertical those of a row.
        explicit separator(ui::orientation along = orientation::horizontal, float margin = 4.0f);

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_paint(painter &p) override;

    private:
        ui::orientation m_along;
        float m_margin;
    };

    math::vec4f surface_color(const struct theme &t, surface s) noexcept;

} // namespace volt::ui
