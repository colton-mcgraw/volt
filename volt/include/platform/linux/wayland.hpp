#pragma once

#include <wayland-client.h>

#include <thread>

// Objects of the xdg-shell and xdg-decoration protocol extensions. Their
// headers are generated at build time and private to the backend, so they are
// only named here.
struct xdg_wm_base;
struct zxdg_decoration_manager_v1;

namespace platform::wayland
{

    // The seat's pointer and keyboard and what they are focused on. Defined in
    // the backend (src/platform/linux/input.cpp).
    struct input;

    // Everything the Wayland backend keeps per application. Filled in by
    // platform::initialize(), never modified afterwards, and owned by the
    // application object. It is a bundle of handles, so copies refer to the
    // same connection.
    struct state
    {
        // Connection to the compositor: every request and event goes through it.
        wl_display *display = nullptr;

        // Global that creates surfaces, the base object of every window.
        wl_compositor *compositor = nullptr;

        // Global that turns a surface into a desktop window (xdg-shell).
        xdg_wm_base *shell = nullptr;

        // Global for buffers in shared memory. Windows use it for the
        // placeholder they show until a renderer draws into them. Null if the
        // compositor does not offer it.
        wl_shm *shm = nullptr;

        // Global through which a window asks the compositor to draw its title
        // bar and borders. Null on compositors that leave decorations to the
        // application (GNOME); windows are undecorated there.
        zxdg_decoration_manager_v1 *decorations = nullptr;

        // Mouse and keyboard. Null if the compositor has no seat.
        wayland::input *input = nullptr;

        // Thread that called platform::initialize(). The display's default
        // event queue is dispatched on this thread only.
        std::thread::id thread;

        // An eventfd that platform::wake() writes to and platform::poll_events()
        // polls alongside the display's socket. Its counter stays nonzero
        // until poll_events() reads it, so a wake is kept until then, and any
        // number of them are one read. -1 until initialized.
        int wake = -1;
    };

} // namespace platform::wayland
