#pragma once

#include <app/ui/input.hpp>
#include <math/vector.hpp>
#include <platform/platform.hpp>

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace platform
{
    // The backend's side of a window: its OS handles. Defined in
    // <platform/windows/window.hpp> and <platform/linux/window.hpp>.
    struct native_window;
} // namespace platform

namespace volt::ui
{

    struct window_configuration
    {
        // Where to put the top-left corner of the content area, in screen
        // coordinates: the window system's units, like get_position().
        // Positions are not scaled, because the desktop has no single scale
        // when its monitors differ. Empty lets the window system choose,
        // which is also the only thing Wayland supports. On Windows the
        // window is then kept inside the work area of the monitor it lands
        // on, moved and if need be made smaller (fit_inside()), whether the
        // position was given or chosen: a remembered position may be on a
        // monitor that has gone, or under a taskbar that has moved.
        std::optional<math::vec2i> position;

        // Size of the content area (the part the application draws), in
        // logical units: the window is created at this size times the content
        // scale of the monitor it lands on, so 1280x720 looks the same size on
        // every display. get_size() then reports the result in the window
        // system's units.
        math::vec2i size{1280, 720};

        // The smallest content area the user may shrink the window to, in
        // logical units like `size` (see window::set_min_size()). Zero on an
        // axis leaves that axis to the window system.
        math::vec2i min_size{};

        std::string title;
        bool resizable = true;
        bool fullscreen = false;
        bool decorated = true;
    };

    // What the monitor a window is on does with color.
    struct display_info
    {
        // The monitor is in HDR mode: it shows colors brighter than SDR white
        // from windows that send them.
        bool hdr = false;

        // The luminance the system gives SDR white on it, in nits. In HDR
        // mode this is what the user set as the brightness of SDR content,
        // and what an HDR window has to match to sit among the others.
        float sdr_white = 80.0f;

        friend bool operator==(const display_info &, const display_info &) = default;
    };

    // A rectangle of the desktop, in screen coordinates: the window system's
    // units, like window::get_position().
    struct screen_rect
    {
        math::vec2i position{};
        math::vec2i size{};

        friend bool operator==(const screen_rect &, const screen_rect &) = default;
    };

    // The part of a monitor windows should go in: its work area, the
    // monitor less the taskbar and other docked bars.
    struct monitor_area
    {
        screen_rect work;

        // The monitor's content scale (window::content_scale() of a window
        // on it).
        float scale = 1.0f;
    };

    // The work area of the monitor `point` (screen coordinates) is on, or of
    // the one nearest to it; without a point, of the primary monitor. Empty
    // where the window system does not let applications place windows
    // (Wayland), or cannot say. Platform thread.
    std::optional<monitor_area> monitor_at(const platform::state &platform,
                                           std::optional<math::vec2i> point = std::nullopt);

    // `window` (a window's outer rectangle, frame included) moved, and first
    // shrunk on each axis where it is larger, so that the whole of it lies
    // inside `area`. What window::create() does with every new window, on
    // the work area of the monitor it lands on, so that no window opens
    // partly under the taskbar or off the desktop. An empty area changes
    // nothing.
    constexpr screen_rect fit_inside(const screen_rect &window, const screen_rect &area) noexcept
    {
        if (area.size.x() <= 0 || area.size.y() <= 0)
            return window;

        screen_rect fitted;
        for (std::size_t axis = 0; axis < 2; ++axis)
        {
            const int size = window.size[axis] < area.size[axis] ? window.size[axis] : area.size[axis];
            const int lowest = area.position[axis];
            const int highest = area.position[axis] + area.size[axis] - size;
            int position = window.position[axis];
            if (position > highest)
                position = highest;
            if (position < lowest)
                position = lowest;
            fitted.size[axis] = size;
            fitted.position[axis] = position;
        }
        return fitted;
    }

    // A logical size in the window system's units at `scale`, rounded to
    // the nearest unit, as window_configuration::size and min_size are
    // turned into pixels on Windows. Zero and negative stay zero.
    constexpr math::vec2i logical_to_window(math::vec2i logical, float scale) noexcept
    {
        math::vec2i result{};
        for (std::size_t axis = 0; axis < 2; ++axis)
        {
            const float scaled = static_cast<float>(logical[axis]) * scale;
            result[axis] = scaled > 0.0f ? static_cast<int>(scaled + 0.5f) : 0;
        }
        return result;
    }

    // ---------------------------------------------------------------------
    // Callbacks
    // ---------------------------------------------------------------------
    //
    // All of them run on the platform thread, from inside
    // platform::poll_events() or from the window call that caused them (a
    // set_size() reports its resize before it returns). That call may be on
    // another window: focus() on one reports the focus the other one loses,
    // destroy() on the active one the focus the next one gains. A callback
    // may call anything on its window, destroy() included, and may replace
    // itself; it must not delete the window object.

    // `scancode` is the backend's raw code for the key, for keys `key` has no
    // name for. It is not portable.
    using key_callback = std::function<void(key key, int scancode, action action, modifiers mods)>;
    using mouse_callback = std::function<void(mouse_button button, action action, modifiers mods)>;

    // Cursor position relative to the top-left of the content area.
    using cursor_callback = std::function<void(const math::vec2i &position)>;
    using enter_callback = std::function<void(const math::vec2i &position)>;
    using leave_callback = std::function<void(const math::vec2i &position)>;

    // In wheel notches: positive y scrolls up (away from the user), positive x
    // scrolls right. Fractional on touchpads and high-resolution wheels.
    using scroll_callback = std::function<void(double xoffset, double yoffset)>;

    // Text input, one Unicode code point at a time, after the keyboard layout
    // has been applied.
    using char_callback = std::function<void(char32_t codepoint)>;

    // Files dropped on the window, as UTF-8 paths. The span is only valid
    // during the call.
    using drop_callback = std::function<void(std::span<const std::string> paths)>;

    using resize_callback = std::function<void(const math::vec2i &size)>;
    using move_callback = std::function<void(const math::vec2i &position)>;

    // The window's content scale changed (see window::content_scale()): it
    // moved to a monitor with another scale, or the user rescaled its
    // monitor. On Windows the window then takes the size that keeps its
    // apparent size, and the scale is reported first, before the resize that
    // follows from it: whoever lays out on the resize already has the scale
    // to lay out with. Both arrive within the same OS message.
    using scale_callback = std::function<void(float scale)>;
    using focus_callback = std::function<void(bool focused)>;
    using maximize_callback = std::function<void(bool maximized)>;
    using minimize_callback = std::function<void(bool minimized)>;

    // The window went back to its normal size from maximized or minimized.
    using restore_callback = std::function<void()>;

    // The user asked to close the window (close button, Alt+F4). Nothing
    // happens unless the callback does it: the window stays open until
    // destroy().
    using close_callback = std::function<void()>;

    // The window lost the mouse while a button was held: another window (a
    // system dialog, a drag-and-drop, the debugger) took the capture
    // without the keyboard focus leaving. The releases of the buttons held
    // will not come; whatever the press began has to be called off, not
    // completed. Windows only: Wayland ends a pointer grab with a leave.
    using capture_lost_callback = std::function<void()>;

    namespace detail
    {
        // The portable half of a window: what the backend last reported, and
        // where it reports to. The window object owns it and the backend holds
        // a pointer to it, which is what pins a window in memory.
        struct window_data
        {
            std::string title;
            math::vec2i position{};
            math::vec2i size{};

            // Window units per logical unit, 1 while the window is closed.
            float scale = 1.0f;

            // The smallest content area, logical units; zero: no limit.
            math::vec2i min_size{};

            bool resizable = true;
            bool fullscreen = false;
            bool decorated = true;
            bool visible = false;
            bool focused = false;
            bool maximized = false;
            bool minimized = false;

            key_callback on_key;
            mouse_callback on_mouse;
            cursor_callback on_cursor;
            enter_callback on_enter;
            leave_callback on_leave;
            scroll_callback on_scroll;
            char_callback on_char;
            drop_callback on_drop;
            resize_callback on_resize;
            move_callback on_move;
            scale_callback on_scale;
            focus_callback on_focus;
            maximize_callback on_maximize;
            minimize_callback on_minimize;
            restore_callback on_restore;
            close_callback on_close;
            capture_lost_callback on_capture_lost;
        };

        // Calls `callback` if one is set. It runs a copy, so that a callback
        // which replaces itself does not destroy the function it is running in.
        template <typename... Args>
        void emit(const std::function<void(Args...)> &callback, std::type_identity_t<Args>... args)
        {
            if (!callback)
                return;

            const auto running = callback;
            running(args...);
        }
    } // namespace detail

    // ---------------------------------------------------------------------
    // window
    // ---------------------------------------------------------------------
    //
    // A top-level window. The object starts out closed; create() opens it and
    // destroy() (or the destructor) closes it again, after which it can be
    // created anew. Windows are created hidden: call show().
    //
    // Everything here must be called on the platform thread, and every window
    // must be destroyed before platform::shutdown(). While a window is closed
    // the setters do nothing and the getters report the last known values
    // (content_scale() excepted, see there).
    //
    // Any number of windows may be open at once. Each reports only its own
    // events, and closing one (close_callback) does nothing to the others or
    // to the application: whether the last window closing ends it is the
    // application's decision.
    //
    // Units. Sizes and positions, those of the callbacks included, are in the
    // window system's own units: physical pixels on Windows, surface-local
    // (logical) units on Wayland. content_scale() says how many of them make
    // one logical unit, which is what an interface lays itself out in. The
    // one exception is window_configuration::size, which is logical, so that
    // a new window comes out the same apparent size wherever it opens.
    //
    // Not everything exists everywhere. On Wayland an application cannot
    // place its windows or learn where they are (set_position() does nothing,
    // the position stays at the origin), is never told about minimizing, and
    // gets a title bar only if the compositor draws one.
    class window
    {
    public:
        window();
        ~window();

        // The backend points back at this object's data.
        window(const window &) = delete;
        window &operator=(const window &) = delete;

        std::expected<void, std::string> create(const platform::state &platform, const window_configuration &config);
        void destroy();

        bool is_open() const noexcept { return m_native != nullptr; }

        // The OS handles, for whoever needs to hand them to another API (the
        // renderer, to create its surface). Null while closed.
        platform::native_window *native() const noexcept { return m_native.get(); }

        void show();
        void hide();

        // maximize() and minimize() also show a hidden window. restore()
        // undoes whichever of the two is in effect.
        void maximize();
        void minimize();
        void restore();

        // Brings the window to the front and gives it the keyboard, showing
        // it if hidden and restoring it if minimized. Windows only lets the
        // application the user is working with take the foreground: from
        // any other, the window's taskbar button flashes instead. On Wayland
        // this only shows a hidden window (see the backend for why).
        void focus();

        void set_title(const std::string &title);
        void set_resizable(bool resizable);
        void set_fullscreen(bool fullscreen);
        void set_decorated(bool decorated);
        void set_position(const math::vec2i &position);

        // In the window system's units, like get_size(), not logical ones:
        // for a logical size, multiply it by content_scale() first.
        void set_size(const math::vec2i &size);

        // The smallest content area the user may drag the window to, in
        // logical units (unlike set_size()), so that it means the same on
        // every monitor: Windows turns it into pixels at the window's
        // current scale, frame added, whenever it is asked
        // (WM_GETMINMAXINFO); Wayland takes it as it is (xdg_toplevel
        // set_min_size). A window smaller than that grows to it. Zero on an
        // axis is no limit. Kept across destroy().
        void set_min_size(const math::vec2i &logical);
        const math::vec2i &min_size() const noexcept { return m_data.min_size; }

        const math::vec2i &get_position() const noexcept { return m_data.position; }
        const math::vec2i &get_size() const noexcept { return m_data.size; }

        // How many of the window's units (get_size(), the callbacks'
        // positions) make one logical unit on the monitor the window is on:
        // its DPI over 96 on Windows, so 1.5 at 150% scaling. 1 while the
        // window is closed, and always on Wayland for now, where the
        // window's units are logical already. scale_callback reports changes.
        float content_scale() const noexcept { return m_data.scale; }
        const std::string &get_title() const noexcept { return m_data.title; }
        bool is_resizable() const noexcept { return m_data.resizable; }
        bool is_fullscreen() const noexcept { return m_data.fullscreen; }
        bool is_decorated() const noexcept { return m_data.decorated; }
        bool is_visible() const noexcept { return m_data.visible; }
        bool is_focused() const noexcept { return m_data.focused; }
        bool is_maximized() const noexcept { return m_data.maximized; }
        bool is_minimized() const noexcept { return m_data.minimized; }

        // The monitor most of the window is on. Cheap while the window stays
        // on the same monitor: the answer is remembered. `refresh` asks the
        // system again, for when the monitor's mode may have changed (nothing
        // announces that; regaining focus is a good moment). SDR while the
        // window is closed, and always on Wayland for now.
        display_info display(bool refresh = false);

        // One callback per event; setting replaces the previous one and an
        // empty function removes it. They survive destroy().
        void set_key_callback(key_callback callback) { m_data.on_key = std::move(callback); }
        void set_mouse_callback(mouse_callback callback) { m_data.on_mouse = std::move(callback); }
        void set_cursor_callback(cursor_callback callback) { m_data.on_cursor = std::move(callback); }
        void set_enter_callback(enter_callback callback) { m_data.on_enter = std::move(callback); }
        void set_leave_callback(leave_callback callback) { m_data.on_leave = std::move(callback); }
        void set_scroll_callback(scroll_callback callback) { m_data.on_scroll = std::move(callback); }
        void set_char_callback(char_callback callback) { m_data.on_char = std::move(callback); }
        void set_drop_callback(drop_callback callback) { m_data.on_drop = std::move(callback); }
        void set_resize_callback(resize_callback callback) { m_data.on_resize = std::move(callback); }
        void set_move_callback(move_callback callback) { m_data.on_move = std::move(callback); }
        void set_scale_callback(scale_callback callback) { m_data.on_scale = std::move(callback); }
        void set_focus_callback(focus_callback callback) { m_data.on_focus = std::move(callback); }
        void set_maximize_callback(maximize_callback callback) { m_data.on_maximize = std::move(callback); }
        void set_minimize_callback(minimize_callback callback) { m_data.on_minimize = std::move(callback); }
        void set_restore_callback(restore_callback callback) { m_data.on_restore = std::move(callback); }
        void set_close_callback(close_callback callback) { m_data.on_close = std::move(callback); }
        void set_capture_lost_callback(capture_lost_callback callback)
        {
            m_data.on_capture_lost = std::move(callback);
        }

    private:
        detail::window_data m_data;

        // Null while the window is closed.
        std::unique_ptr<platform::native_window> m_native;
    };

} // namespace volt::ui
