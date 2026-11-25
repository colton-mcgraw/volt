#pragma once

#include <app/ui/input.hpp>
#include <math/vector.hpp>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

// -------------------------------------------------------------------------
// input events
// -------------------------------------------------------------------------
//
// What <app/ui/input/processor.hpp> makes of the window's callbacks, as plain
// structs for the event bus (<events/bus.hpp>): any of them can be listened
// for by type. The raw ones mirror a callback each; the derived ones (click,
// drag) are worked out from several.
//
// Positions are in the window system's units, relative to the top-left of the
// content area, like the window's own (physical pixels on Windows, logical
// units on Wayland). A position outside the content area, negative included,
// is possible while a button is held: the window keeps the pointer then.
//
// Every event carries the time its callback ran, by the processor's clock.

namespace volt::ui
{

    using input_clock = std::chrono::steady_clock;
    using input_time = input_clock::time_point;

    // ---- keyboard ---------------------------------------------------------

    // A key went down, auto-repeated, or came up. Presses and releases pair
    // up: a release always follows a press of the same key, a press of a key
    // that is already down arrives as a repeat, and losing focus releases
    // every key that is held (`synthetic`). Keys `key` has no name for
    // (key::unknown) are passed through as reported, unpaired.
    //
    // `mods` already accounts for the key itself: pressing Shift reports
    // shift, releasing it does not (unless the other Shift is still down).
    struct key_event
    {
        ui::key key = ui::key::unknown;
        int scancode = 0; // the backend's code, not portable; see window.hpp
        ui::action action = ui::action::press;
        modifiers mods = modifiers::none;
        bool synthetic = false; // released by focus loss, not by the user
        input_time time{};
    };

    // One code point of typed text, after the keyboard layout. Control
    // characters (C0, DEL, C1) never arrive here: Enter, Tab and Backspace
    // are keys.
    struct text_event
    {
        char32_t codepoint = 0;
        modifiers mods = modifiers::none;
        input_time time{};
    };

    // ---- mouse -------------------------------------------------------------
    //
    // The mouse events carry the cursor position, the modifiers held, and the
    // buttons held, as of the event. While any button is held the pointer is
    // captured: motion keeps arriving wherever the cursor goes, and leaving
    // the window is announced only once the last button is released.

    // A button went down or up. Presses and releases pair up as keys do; a
    // release by focus loss is `synthetic` and never makes a click.
    //
    // `clicks` counts the presses in a row, 2 for the second press of a
    // double click; a release carries the count of its press. See
    // gesture_settings for what "in a row" means.
    //
    // A release comes after what it completes: the click_event or
    // drag_end_event for that button precedes it, while the button still
    // shows as held, so a capture still routes them to the press's target.
    struct mouse_button_event
    {
        mouse_button button = mouse_button::left;
        ui::action action = ui::action::press;
        math::vec2i position{};
        modifiers mods = modifiers::none;
        mouse_buttons buttons = mouse_buttons::none; // held after this event
        int clicks = 1;
        bool synthetic = false;
        input_time time{};
    };

    // The cursor moved. Only sent when the position changes. `delta` is from
    // the position of the previous mouse event, and is never a jump across
    // the screen: entering the window starts afresh from the entry point.
    struct mouse_move_event
    {
        math::vec2i position{};
        math::vec2i delta{};
        modifiers mods = modifiers::none;
        mouse_buttons buttons = mouse_buttons::none;
        input_time time{};
    };

    // The cursor came over the content area.
    struct mouse_enter_event
    {
        math::vec2i position{};
        modifiers mods = modifiers::none;
        input_time time{};
    };

    // The cursor left the content area, with no button held (see above).
    struct mouse_leave_event
    {
        math::vec2i position{};
        modifiers mods = modifiers::none;
        input_time time{};
    };

    // In wheel notches: positive y scrolls up (away from the user), positive x
    // to the right. Fractional on touchpads and high-resolution wheels.
    // `position` is the cursor's, for routing to what is under it.
    struct scroll_event
    {
        math::vec2d offset{};
        math::vec2i position{};
        modifiers mods = modifiers::none;
        mouse_buttons buttons = mouse_buttons::none;
        input_time time{};
    };

    // ---- derived mouse gestures --------------------------------------------

    // A button was pressed and released without the cursor moving further
    // than the drag threshold. `clicks` is the count of the press, so a
    // double click is a click with clicks == 2 (preceded by one with 1).
    struct click_event
    {
        mouse_button button = mouse_button::left;
        math::vec2i position{};
        int clicks = 1;
        modifiers mods = modifiers::none;
        input_time time{};
    };

    // The first button of a capture was held and the cursor moved beyond the
    // drag threshold from where it was pressed. `origin` is the press position.
    // A drag always ends with a drag_end_event, then no click_event follows.
    struct drag_begin_event
    {
        mouse_button button = mouse_button::left;
        math::vec2i origin{};
        math::vec2i position{};
        modifiers mods = modifiers::none;
        input_time time{};
    };

    // The cursor moved during a drag. `delta` is from the previous drag event.
    struct drag_move_event
    {
        mouse_button button = mouse_button::left;
        math::vec2i origin{};
        math::vec2i position{};
        math::vec2i delta{};
        modifiers mods = modifiers::none;
        input_time time{};
    };

    // The drag's button was released, or focus was lost (`cancelled`), in
    // which case whatever the drag was doing should be undone.
    struct drag_end_event
    {
        mouse_button button = mouse_button::left;
        math::vec2i origin{};
        math::vec2i position{};
        modifiers mods = modifiers::none;
        bool cancelled = false;
        input_time time{};
    };

    // Files dropped on the window, as UTF-8 paths, at the cursor position.
    struct drop_event
    {
        std::vector<std::string> paths;
        math::vec2i position{};
        modifiers mods = modifiers::none;
        input_time time{};
    };

    // ---- window ------------------------------------------------------------

    // The content area changed size.
    struct window_resize_event
    {
        math::vec2i size{};
        input_time time{};
    };

    // The window moved on the screen. Never sent on Wayland.
    struct window_move_event
    {
        math::vec2i position{};
        input_time time{};
    };

    // The window's content scale changed: how many of the window's units
    // (the sizes and positions in these events) make one logical unit, which
    // is what an interface lays itself out in. See window::content_scale().
    //
    // When a change of scale also resizes the window (Windows keeps its
    // apparent size), this comes first and the window_resize_event follows,
    // so that a layout on the resize uses the new scale. Attaching to a
    // window (window_input::attach) reports its scale with one of these,
    // unless it is the scale input_state already has: 1 to begin with.
    struct window_scale_event
    {
        float scale = 1.0f;
        input_time time{};
    };

    // The window gained or lost the keyboard. On a loss everything that was
    // held has been released (by synthetic events) before this arrives.
    struct focus_event
    {
        bool focused = false;
        input_time time{};
    };

    enum class window_state
    {
        normal,
        maximized,
        minimized,
    };

    // The window was maximized, minimized or restored. Sent when the state
    // changes; a window going straight from minimized to maximized may pass
    // through normal on the way, as the platform reports it.
    struct window_state_event
    {
        window_state state = window_state::normal;
        input_time time{};
    };

    // The user asked to close the window. Nothing happens unless a listener
    // does it.
    struct close_request_event
    {
        input_time time{};
    };

    // ---- all of them --------------------------------------------------------

    // Every event the processor produces, for storing them in order.
    using input_event =
        std::variant<key_event, text_event, mouse_button_event, mouse_move_event, mouse_enter_event,
                     mouse_leave_event, scroll_event, click_event, drag_begin_event, drag_move_event, drag_end_event,
                     drop_event, window_resize_event, window_move_event, window_scale_event, focus_event,
                     window_state_event, close_request_event>;

    inline input_time time_of(const input_event &event) noexcept
    {
        return std::visit([](const auto &e) { return e.time; }, event);
    }

} // namespace volt::ui
