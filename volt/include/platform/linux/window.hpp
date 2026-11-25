#pragma once

#include <math/vector.hpp>
#include <platform/linux/wayland.hpp>

#include <memory>

namespace platform::wayland
{
    // The zenity or kdialog a window's file dialog runs as, and the thread
    // that waits for it. Defined in the backend (src/platform/linux/
    // file_dialog.cpp); destroying it ends the program and joins the thread.
    struct file_chooser;
} // namespace platform::wayland

struct xdg_surface;
struct xdg_toplevel;
struct zxdg_toplevel_decoration_v1;

namespace volt::ui::detail
{
    struct window_data;
} // namespace volt::ui::detail

namespace platform
{

    // The Wayland side of a volt::ui::window. Owned by the window object and
    // reachable from the wl_surface (its user data points here), which is how
    // input events find the window they belong to.
    struct native_window
    {
        // The connection the window lives on.
        wayland::state platform;

        // A window is three objects stacked on each other: the surface holds
        // the pixels, the xdg_surface makes it part of the desktop, and the
        // xdg_toplevel gives it the role of an application window.
        wl_surface *surface = nullptr;
        xdg_surface *shell_surface = nullptr;
        xdg_toplevel *toplevel = nullptr;

        // Null when the compositor does not offer server-side decorations.
        zxdg_toplevel_decoration_v1 *decoration = nullptr;

        // The portable state and callbacks, owned by the window object.
        volt::ui::detail::window_data *data = nullptr;

        // The compositor has answered the commit that asked for the window to
        // be mapped. Buffers may only be attached from then on.
        bool configured = false;

        // What the compositor announced with xdg_toplevel.configure. It takes
        // effect when the xdg_surface.configure that follows is acknowledged.
        struct
        {
            volt::math::vec2i size{};
            bool maximized = false;
            bool fullscreen = false;
            bool activated = false;
        } pending;

        // Size to use when the compositor leaves the choice to the window,
        // which it does for a window that is neither maximized nor fullscreen.
        volt::math::vec2i windowed_size{};

        // Made by the first file dialog (<platform/file_dialog.hpp>). Shared
        // only so that it can be destroyed where its type is unknown: this
        // is the one owner.
        std::shared_ptr<wayland::file_chooser> file_chooser;
    };

} // namespace platform
