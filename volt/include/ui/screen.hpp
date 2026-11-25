#pragma once

#include <app/ui/input/capture.hpp>
#include <app/ui/input/events.hpp>
#include <app/ui/input/shortcut.hpp>
#include <events/bus.hpp>
#include <events/token.hpp>
#include <ui/clipboard.hpp>
#include <ui/draw_list.hpp>
#include <ui/geometry.hpp>
#include <ui/text.hpp>
#include <ui/theme.hpp>
#include <ui/widget.hpp>

#include <math/vector.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// screen
// -------------------------------------------------------------------------
//
// Shows a widget tree in a window: lays it out, routes the window's input
// to it, keeps the focus, opens popups over it, runs its timers and paints
// it into a draw list. The application feeds it input events and asks it
// for a draw list when it wants a frame; the screen tells it, through the
// redraw callback, when it needs one.
//
// Threading. A screen and its widgets belong to one thread, the interface
// thread; in this application that is the render thread, which drains the
// window's queued input (<app/ui/input/window_input.hpp>) at the start of a
// frame, so input, layout and painting never race. The one exception is
// post(): any thread may hand the interface thread a function to run.
//
// Units. Input arrives in the window's pixels and the draw list is in
// pixels; in between everything is in logical units, pixels divided by the
// scale (set_scale()). At a scale of 2 the interface is twice as many
// pixels in every direction, its text rasterized at twice the size.
//
// Routing, by kind of event:
//
//   * press: to the widget under the pointer, bubbling up until one handles
//     it. That one captures the pointer: every pointer event up to the
//     release of the last button goes to it, wherever the pointer goes
//     (pointer_capture, <app/ui/input/capture.hpp>). A press on a focusable
//     widget focuses it first.
//   * move: to the captured widget, or else the one under the pointer. Not
//     bubbled. Without a capture the hover changes: widgets the pointer
//     leaves get on_pointer_leave(), those it enters on_pointer_enter(),
//     ancestors included (hovered() is true along the whole chain).
//   * click and drag: to the captured widget, or bubbling from the one under
//     the pointer when nothing captured the press.
//   * wheel: bubbling from the widget under the pointer, whatever is held,
//     so it scrolls the innermost scrollable thing there.
//   * key and text: bubbling from the focused widget. A key nobody handled
//     is then Escape (closes the topmost popup), Tab and Shift+Tab (move
//     the focus), or a shortcut (shortcuts()).
//
// Popups (menus, dropdown lists, tooltips, dialogs) lie over the tree in the
// order opened. They get the pointer first. A press outside every popup
// closes the ones that close on outside clicks and is swallowed, along with
// the rest of its gesture; a press inside one closes those opened after it.
// A modal popup draws a backdrop and takes all input: nothing under it is
// reached until it closes. Closing a popup destroys its widgets once the
// event being handled is done, so a menu item may close its own menu.
//
// Time: timers (schedule()) and animations (widget::animate()) run in
// update(). next_wakeup() says when the next of them is due, so the
// application can sleep until then instead of drawing frames in a loop.

namespace volt::ui
{

    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    enum class placement : std::uint8_t
    {
        below,    // under the anchor, left edges aligned (dropdowns, menus)
        right,    // beside the anchor, top edges aligned (submenus)
        point,    // with its top-left corner at the anchor's (context menus, tooltips)
        centered, // in the middle of the screen (dialogs)
    };

    struct popup_options
    {
        rect anchor;
        ui::placement placement = placement::below;

        // Takes all input while open and darkens what is under it.
        bool modal = false;

        // Closes on a press outside it (and Escape).
        bool dismissable = true;

        // Gets the keyboard focus while open; it goes back where it was
        // when the popup closes.
        bool take_focus = true;

        // Lets the pointer through: tooltips.
        bool hit_testable = true;

        // At least as wide as the anchor.
        bool match_anchor_width = false;

        // The widget that opened it, for whom a press on it should close
        // the popup and do nothing else (pressing a dropdown's button
        // again closes its list rather than opening it anew).
        widget *owner = nullptr;

        std::function<void()> closed;
    };

    class screen
    {
    public:
        explicit screen(text_engine &text, ui::theme theme = ui::theme::dark_theme());
        ~screen();

        screen(const screen &) = delete;
        screen &operator=(const screen &) = delete;

        // ---- the tree ------------------------------------------------------------

        // Replaces the root; the old one is destroyed.
        void set_root(std::unique_ptr<widget> root);
        widget *root() const noexcept { return m_root.get(); }

        template <typename W, typename... Args>
        W &emplace_root(Args &&...args)
        {
            auto made = std::make_unique<W>(std::forward<Args>(args)...);
            W &result = *made;
            set_root(std::move(made));
            return result;
        }

        // ---- services ------------------------------------------------------------

        const ui::theme &theme() const noexcept { return m_theme; }
        void set_theme(ui::theme theme);

        text_engine &text() const noexcept { return m_text; }

        // Defaults to a memory_clipboard. Not owned; must outlive the screen.
        ui::clipboard &clipboard() const noexcept { return *m_clipboard; }
        void set_clipboard(ui::clipboard *clipboard) noexcept;

        // ---- size and scale ---------------------------------------------------

        // The window's content area in pixels.
        void set_size(const math::vec2i &pixels);

        // Pixels per logical unit: 1, 1.25, 1.5, 2...
        void set_scale(float scale);
        float scale() const noexcept { return m_scale; }

        // The content area in logical units.
        math::vec2f size() const noexcept { return m_size; }

        // ---- frames --------------------------------------------------------------

        // Called whenever the screen starts to need a frame. On the interface
        // thread, except that post() calls it on the posting thread; so it
        // must be cheap and thread safe (the application sets a flag and
        // wakes its render thread).
        void set_redraw_callback(std::function<void()> callback);

        bool needs_redraw() const noexcept { return m_needs_redraw; }
        void request_redraw();

        // Runs posted functions, due timers and animations, as of `now`.
        // Once per frame, before input is handled and before paint().
        void update(time_point now);
        time_point now() const noexcept { return m_now; }

        // When update() next has something to do, if anything: the earliest
        // timer, or the next animation frame.
        std::optional<time_point> next_wakeup() const;

        // Lays out what needs it.
        void layout();

        // Lays out, then fills `list` with everything on screen, popups
        // included, in pixels. Clears needs_redraw(). If the text engine's
        // atlas overflowed while painting, it is reset and everything is
        // painted again, so the list never refers to glyphs the reset lost.
        void paint(draw_list &list);

        // ---- time ----------------------------------------------------------------

        using timer_id = std::uint64_t;

        // Runs `callback` in the first update() at or after `when`.
        timer_id schedule(time_point when, std::function<void()> callback);
        timer_id schedule_after(clock::duration delay, std::function<void()> callback)
        {
            return schedule(m_now + delay, std::move(callback));
        }
        void cancel(timer_id id) noexcept;

        // From any thread: runs `task` on the interface thread in the next
        // update(), and asks for one.
        void post(std::function<void()> task);

        // ---- input -----------------------------------------------------------------

        // Each returns whether a widget (or a shortcut) handled the event.
        bool handle(const mouse_button_event &e);
        bool handle(const mouse_move_event &e);
        bool handle(const mouse_enter_event &e);
        bool handle(const mouse_leave_event &e);
        bool handle(const scroll_event &e);
        bool handle(const click_event &e);
        bool handle(const drag_begin_event &e);
        bool handle(const drag_move_event &e);
        bool handle(const drag_end_event &e);
        bool handle(const key_event &e);
        bool handle(const text_event &e);
        bool handle(const focus_event &e);

        // Listens on `bus` for every event above, handling each where the
        // bus dispatches it: for window_input's queued bus, in drain(). The
        // tokens remove the listeners again.
        std::vector<events::scoped_token> connect(events::bus &bus);

        // ---- focus -------------------------------------------------------------------

        widget *focused() const noexcept { return m_focus; }
        void set_focus(widget *target, focus_reason reason = focus_reason::programmatic);

        // Whether to show the focus ring: the focus came from the keyboard.
        bool focus_visible() const noexcept { return m_focus_visible; }

        // Tab order: depth first through the tree (or the topmost modal
        // popup), visible and enabled focusable widgets only, wrapping.
        bool focus_next(bool backwards = false);

        // ---- pointer -----------------------------------------------------------------

        // The deepest widget the pointer is over, popups first.
        widget *hovered() const noexcept { return m_hover.empty() ? nullptr : m_hover.back(); }
        widget *captured() const noexcept;
        void capture(widget &target);
        void release_capture() noexcept;

        // What the pointer should look like, and a callback for when that
        // changes. The platform layer has no way to set the cursor yet; when
        // it gets one, this callback is what calls it.
        cursor_shape cursor() const noexcept { return m_cursor; }
        void set_cursor_callback(std::function<void(cursor_shape)> callback)
        {
            m_cursor_callback = std::move(callback);
        }

        // The pointer's last position, logical units.
        math::vec2f pointer() const noexcept { return m_pointer; }

        // The widget at `point`, popups first; null where there is none.
        widget *hit_test(const math::vec2f &point) const;

        // ---- popups ------------------------------------------------------------------

        // Shows `content` over everything else and returns it.
        widget &open_popup(std::unique_ptr<widget> content, popup_options options);

        template <typename W>
        W &open(std::unique_ptr<W> content, popup_options options)
        {
            W &result = *content;
            open_popup(std::move(content), std::move(options));
            return result;
        }

        // Closes the popup whose content is `content` (or contains it), and
        // every popup opened after it.
        void close_popup(widget &content);
        void close_all_popups();
        bool is_open(const widget &content) const noexcept;
        std::size_t popup_count() const noexcept { return m_popups.size(); }
        widget *popup(std::size_t index) const noexcept { return m_popups[index].content.get(); }

        // ---- commands and shortcuts -----------------------------------------------------

        // Named actions that menus and shortcuts run, so a shortcut and its
        // menu item do the same thing and the item can show the shortcut.
        void add_command(std::string id, std::function<void()> action);
        bool run_command(std::string_view id);
        bool has_command(std::string_view id) const;

        shortcut_table &shortcuts() noexcept { return m_shortcuts; }

        // "Ctrl+S", or "" when the command has no shortcut.
        std::string shortcut_text(std::string_view command) const;

        // ---- for widgets ------------------------------------------------------------

        void invalidate_layout() noexcept;
        bool layout_dirty() const noexcept { return m_layout_dirty; }
        void start_animation(widget &target);

        // A widget leaves the tree: forget every reference to it.
        void forget(widget &gone) noexcept;

    private:

        struct popup_entry
        {
            std::unique_ptr<widget> content;
            popup_options options;
            widget *previous_focus = nullptr;
        };

        struct timer
        {
            timer_id id = 0;
            time_point when{};
            std::function<void()> callback;
        };

        math::vec2f logical(const math::vec2i &pixels) const noexcept;
        pointer_event pointer_from(const math::vec2f &position, modifiers mods) const noexcept;

        // The popups that input may reach: from the topmost modal one up.
        std::size_t first_reachable_popup() const noexcept;
        int popup_index_of(const widget *w) const noexcept;

        void update_hover(widget *target);
        void update_cursor();
        void arrange_popup(popup_entry &entry);

        // Delivers along the chain from `target` up until `handler` returns
        // true; returns who did.
        template <typename F>
        widget *bubble(widget *target, F &&handler);

        bool handle_unhandled_key(const key_event &e);
        void close_popups_from(std::size_t index);
        void bury();

        void hide_tooltip();
        void show_tooltip();

        text_engine &m_text;
        ui::theme m_theme;
        memory_clipboard m_default_clipboard;
        ui::clipboard *m_clipboard = &m_default_clipboard;

        std::unique_ptr<widget> m_root;
        std::vector<popup_entry> m_popups;
        std::vector<std::unique_ptr<widget>> m_graveyard;

        math::vec2i m_pixels{};
        math::vec2f m_size{};
        float m_scale = 1.0f;

        bool m_layout_dirty = true;
        bool m_needs_redraw = true;
        std::function<void()> m_redraw_callback;

        time_point m_now{};
        time_point m_last_animation{};
        std::vector<timer> m_timers;
        timer_id m_last_timer = 0;
        std::vector<widget *> m_animating;

        std::mutex m_posted_mutex;
        std::vector<std::function<void()>> m_posted;

        widget *m_focus = nullptr;
        bool m_focus_visible = false;

        pointer_capture<widget *> m_capture;
        std::vector<widget *> m_hover; // root first, deepest last
        math::vec2f m_pointer{};
        bool m_pointer_inside = false;
        bool m_swallowing = false; // the press closed popups: ignore the rest of its gesture

        cursor_shape m_cursor = cursor_shape::arrow;
        std::function<void(cursor_shape)> m_cursor_callback;

        // The tooltip: which widget's, and the timer that shows it.
        widget *m_tooltip_for = nullptr;
        widget *m_tooltip = nullptr;
        timer_id m_tooltip_timer = 0;

        std::map<std::string, std::function<void()>, std::less<>> m_commands;
        shortcut_table m_shortcuts;
    };

} // namespace volt::ui
