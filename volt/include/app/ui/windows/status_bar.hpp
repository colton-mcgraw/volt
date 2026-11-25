#pragma once

#include <ui/screen.hpp>
#include <ui/widgets/bars.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

// -------------------------------------------------------------------------
// window_status
// -------------------------------------------------------------------------
//
// The status bar every window has, and what the window manager and the
// window's host say in it. Left to right:
//
//   [the content's own segments] | message ......... [progress] | frame
//
// The content adds its segments (cursor position, zoom, ...) through
// window_layout::status; the rest is here:
//
//   * the message: show_message(), cleared after a while. Each window has
//     its own; app_services::show_message() picks the window.
//   * the progress bar: shown while files are read or the simulation runs
//     in a window that shows it (set_busy(), from the render loop).
//   * the frame: the window's size in pixels, the display's encoding, and
//     the interface scale (set_frame_info()). No frame counter: a text that
//     changes every frame would ask for a frame after every frame.
//
// Interface thread. Owned by the window's host; its labels by the screen's
// tree, which outlives neither.

namespace volt::ui
{

    class label;
    class progress_bar;

    class window_status
    {
    public:
        explicit window_status(ui::screen &screen) : m_screen(screen) {}
        ~window_status();

        window_status(const window_status &) = delete;
        window_status &operator=(const window_status &) = delete;

        // The bar, with the content's segments added by `own` (may be
        // empty) at the left.
        std::unique_ptr<status_bar> build(const std::function<void(status_bar &)> &own);

        void show_message(std::string text, std::chrono::seconds duration = std::chrono::seconds(4));
        void set_frame_info(std::string text);
        void set_busy(bool busy);

    private:
        ui::screen &m_screen;
        label *m_message = nullptr;
        progress_bar *m_progress = nullptr;
        label *m_frame = nullptr;
        screen::timer_id m_timer = 0;
    };

} // namespace volt::ui
