#pragma once

#include <app/ui/window.hpp>
#include <app/ui/window_book.hpp>
#include <math/vector.hpp>

#include <cmath>
#include <string>
#include <string_view>

// -------------------------------------------------------------------------
// window geometry and titles
// -------------------------------------------------------------------------
//
// The arithmetic of where the application's windows open, how small they
// may get and what their title bars say: plain functions, so that they are
// tested without a window (volt/tests/app). The window manager (<app/ui/
// window_manager.hpp>) applies them; the platform keeps every window inside
// its monitor's work area on top (window_configuration::position).

namespace volt::ui
{

    // What a window's title bar (and the taskbar) says: the window, and
    // the open project with an asterisk while it has unsaved changes, the
    // project first so that a row of taskbar buttons tells projects apart.
    //
    //   Volt — Schematic Editor               no project
    //   Blinky — Schematic Editor — Volt      a project
    //   *Blinky — Schematic Editor — Volt     with unsaved changes
    inline std::string window_title(window_kind kind, std::string_view project, bool modified)
    {
        constexpr std::string_view dash = " \xE2\x80\x94 ";
        if (project.empty())
            return "Volt" + std::string(dash) + std::string(window_name(kind));
        std::string title = modified ? "*" : "";
        title += project;
        title += dash;
        title += window_name(kind);
        title += dash;
        title += "Volt";
        return title;
    }

    // A size the interface needs, in the interface's logical units (what a
    // window content gives as its default or minimum size), as the window's
    // logical size: the user scale (<app/ui/scale.hpp>) draws the interface
    // that much bigger in the same window, so the window grows with it to
    // show as much. Rounded up, so that a minimum stays one.
    inline math::vec2i with_user_scale(math::vec2i logical, float user_scale)
    {
        const float scale = user_scale > 0.0f ? user_scale : 1.0f;
        return {static_cast<int>(std::ceil(static_cast<float>(logical.x()) * scale - 1e-3f)),
                static_cast<int>(std::ceil(static_cast<float>(logical.y()) * scale - 1e-3f))};
    }

    // Where a window of `kind` that has no remembered place opens: the top
    // left of its content area, in screen coordinates, so that a content
    // area `logical` big at the monitor's scale sits in the middle of
    // `area`, each kind a step further down and right so that windows opened
    // together do not cover each other's title bars exactly. The same place
    // every time, unlike the system's cascade, which moves on with every
    // window made. The platform then keeps the frame inside the work area,
    // shrinking a window too big for it (fit_inside()).
    inline math::vec2i default_position(const monitor_area &area, math::vec2i logical, window_kind kind)
    {
        const float scale = area.scale > 0.0f ? area.scale : 1.0f;
        const math::vec2i size = logical_to_window(logical, scale);
        const int step = static_cast<int>(std::lround(32.0f * scale)) * static_cast<int>(index_of(kind));
        const int x = area.work.position.x() + (area.work.size.x() - size.x()) / 2 + step;
        const int y = area.work.position.y() + (area.work.size.y() - size.y()) / 2 + step;
        // Never above or left of the work area, where the title bar would
        // be out of reach; the platform takes care of the other two sides.
        return {x > area.work.position.x() ? x : area.work.position.x(),
                y > area.work.position.y() ? y : area.work.position.y()};
    }

} // namespace volt::ui
