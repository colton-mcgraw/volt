#pragma once

#include <app/ui/input.hpp>
#include <app/ui/input/events.hpp>
#include <app/ui/input/state.hpp>
#include <math/vector.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// input_processor
// -------------------------------------------------------------------------
//
// Turns the window's raw callbacks into the events of <app/ui/input/events.hpp>
// and hands each, in order, to one output function. It is where the input is
// made consistent, so that nothing downstream has to defend against the
// platforms:
//
//   * presses and releases pair up, for keys and buttons both, and losing
//     focus releases whatever is held, with synthetic events, so nothing
//     sticks when the release went to another window; so does losing the
//     mouse capture (the buttons only, cancelled: no click comes of them);
//   * modifiers include the modifier key being pressed and exclude the one
//     being released (Wayland reports them one event late);
//   * leaving the window while a button is held is held back until the
//     release (the pointer is captured; see mouse_button_event);
//   * clicks, multi-clicks and drags are recognized here, at the time of the
//     raw input, so that their timing does not depend on when, or on which
//     thread, the events are finally looked at.
//
// The entry points have the signatures of the window's callbacks, so a test
// or a script can feed input exactly as a window would; window_input
// (<app/ui/input/window_input.hpp>) is what connects them to a real window.
//
// Threading: none of its own. Everything, the output function included, runs
// on the thread that calls the entry points, which for a window's input is
// the platform thread. The clock is injected for the tests.

namespace volt::ui
{

    // Thresholds for recognizing gestures, in the units of the positions.
    // The defaults are those of Windows (GetDoubleClickTime, SM_CXDOUBLECLK,
    // SM_CXDRAG); none of them scales with the display yet.
    struct gesture_settings
    {
        // A press continues a series (double, triple click) if it comes this
        // soon after the previous press of the same button...
        std::chrono::milliseconds multi_click_time{500};

        // ...and no further than this from it on either axis.
        int multi_click_distance = 2;

        // The count starts over at 1 after this many; 0 lets it grow without
        // end.
        int max_clicks = 3;

        // Moving further than this from the press, on either axis, makes the
        // press a drag rather than a click.
        int drag_threshold = 4;
    };

    class input_processor
    {
    public:
        using output = std::function<void(input_event &&)>;
        using clock = std::function<input_time()>;

        // `now` defaults to input_clock::now.
        explicit input_processor(output out, gesture_settings settings = {}, clock now = {})
            : m_output(std::move(out)), m_settings(settings), m_now(std::move(now))
        {
            if (!m_now)
                m_now = [] { return input_clock::now(); };
        }

        // State as of the last event output.
        const input_state &state() const noexcept { return m_state; }
        window_state current_window_state() const noexcept { return m_window_state; }

        const gesture_settings &settings() const noexcept { return m_settings; }
        void set_settings(const gesture_settings &settings) noexcept { m_settings = settings; }

        // ---- raw input, as the window's callbacks deliver it ----------------

        void on_key(key which, int scancode, action act, modifiers mods)
        {
            const input_time now = m_now();

            if (which != key::unknown)
            {
                const bool held = m_state.is_down(which);
                if (act == action::repeat && !held)
                    act = action::press; // it was already down when focus came
                else if (act == action::press && held)
                    act = action::repeat;
            }

            mods = with_key(which, act, mods);

            // A release whose press went to another window is of no interest,
            // unless it changes the modifiers.
            if (which != key::unknown && act == action::release && !m_state.is_down(which) && mods == m_state.mods())
                return;

            if (which != key::unknown && act != action::release)
                m_scancodes[index(which)] = scancode;

            emit(key_event{which, scancode, act, mods, false, now});
        }

        void on_mouse(mouse_button button, action act, modifiers mods)
        {
            const input_time now = m_now();
            const bool held = m_state.is_down(button);

            if (act == action::press && !held)
                press(button, mods, now);
            else if (act == action::release && held)
                release(button, mods, now, false);
        }

        void on_cursor(const math::vec2i &position)
        {
            const input_time now = m_now();

            if (!m_state.cursor_inside() && !m_state.captured())
            {
                // Motion without an enter: the enter got lost somewhere.
                emit(mouse_enter_event{position, m_state.mods(), now});
                return;
            }
            move_to(position, now);
        }

        void on_enter(const math::vec2i &position)
        {
            const input_time now = m_now();

            // Back in before a held leave was announced: it never happened.
            if (std::exchange(m_leave_pending, false) || m_state.cursor_inside())
            {
                move_to(position, now);
                return;
            }
            emit(mouse_enter_event{position, m_state.mods(), now});
        }

        void on_leave(const math::vec2i &position)
        {
            const input_time now = m_now();
            if (!m_state.cursor_inside())
                return;

            move_to(position, now);
            if (m_state.captured())
                m_leave_pending = true;
            else
                emit(mouse_leave_event{m_state.cursor(), m_state.mods(), now});
        }

        void on_scroll(double xoffset, double yoffset)
        {
            emit(scroll_event{{xoffset, yoffset}, m_state.cursor(), m_state.mods(), m_state.buttons(), m_now()});
        }

        void on_char(char32_t codepoint)
        {
            // C0 controls, DEL and C1 controls are keys (Windows sends Ctrl+A
            // as 0x01); surrogates and beyond are not code points.
            const bool control = codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0);
            const bool invalid = (codepoint >= 0xD800 && codepoint < 0xE000) || codepoint > 0x10FFFF;
            if (control || invalid)
                return;

            emit(text_event{codepoint, m_state.mods(), m_now()});
        }

        void on_drop(std::span<const std::string> paths)
        {
            emit(drop_event{std::vector<std::string>(paths.begin(), paths.end()), m_state.cursor(), m_state.mods(),
                            m_now()});
        }

        void on_resize(const math::vec2i &size) { emit(window_resize_event{size, m_now()}); }
        void on_move(const math::vec2i &position) { emit(window_move_event{position, m_now()}); }

        // Only changes are sent, and only to a scale that can be laid out
        // with: zero, negative and NaN are dropped.
        void on_scale(float scale)
        {
            if (!(scale > 0.0f) || scale == m_state.scale())
                return;
            emit(window_scale_event{scale, m_now()});
        }

        void on_focus(bool focused)
        {
            if (focused == m_state.focused())
                return;

            const input_time now = m_now();
            if (!focused)
                release_all(now);
            emit(focus_event{focused, now});
        }

        void on_maximize(bool maximized)
        {
            m_maximized = maximized;
            update_window_state();
        }

        void on_minimize(bool minimized)
        {
            m_minimized = minimized;
            update_window_state();
        }

        void on_restore()
        {
            m_maximized = false;
            m_minimized = false;
            update_window_state();
        }

        void on_close() { emit(close_request_event{m_now()}); }

        // Another window took the mouse while buttons were held here, and
        // the keyboard focus stayed (Windows: WM_CAPTURECHANGED). Every held
        // button is released with a synthetic, cancelled event: no click
        // comes of the press, a drag ends cancelled, and a leave held back
        // for the release is announced. Keys and modifiers stay as they
        // are: the keyboard did not go anywhere.
        void on_capture_lost()
        {
            const input_time now = m_now();
            const modifiers mods = m_state.mods();
            for (std::size_t i = 0; i < mouse_button_count; ++i)
            {
                const auto button = static_cast<mouse_button>(i);
                if (m_state.is_down(button) && m_drag_button != button)
                    release(button, mods, now, true);
            }
            if (m_drag_button && m_state.is_down(*m_drag_button))
                release(*m_drag_button, mods, now, true);
        }

        // Releases every held button and key with synthetic events, cancelling
        // a drag, and drops the held modifiers. Losing focus does this; so
        // should anything else after which the releases will not come.
        void release_all() { release_all(m_now()); }

    private:
        // What is remembered about a held button's press.
        struct press_record
        {
            math::vec2i origin{};
            int clicks = 1;
            bool moved = false; // beyond the drag threshold: no click
        };

        // The previous press, to tell whether the next one continues a series.
        struct series
        {
            mouse_button button = mouse_button::left;
            input_time time{};
            math::vec2i position{};
            int clicks = 0; // 0: there is no series to continue
        };

        static constexpr std::size_t index(key k) noexcept { return static_cast<std::size_t>(k); }
        static constexpr std::size_t index(mouse_button b) noexcept { return static_cast<std::size_t>(b); }

        static int distance(const math::vec2i &a, const math::vec2i &b) noexcept
        {
            const math::vec2i d = a - b;
            const int x = d.x() < 0 ? -d.x() : d.x();
            const int y = d.y() < 0 ? -d.y() : d.y();
            return x > y ? x : y;
        }

        static constexpr key twin(key k) noexcept
        {
            switch (k)
            {
            case key::left_shift: return key::right_shift;
            case key::right_shift: return key::left_shift;
            case key::left_control: return key::right_control;
            case key::right_control: return key::left_control;
            case key::left_alt: return key::right_alt;
            case key::right_alt: return key::left_alt;
            case key::left_super: return key::right_super;
            case key::right_super: return key::left_super;
            default: return key::unknown;
            }
        }

        template <typename Event>
        void emit(Event &&e)
        {
            m_state.apply(e);
            m_output(input_event{std::forward<Event>(e)});
        }

        // `reported` corrected for the key `which` itself, see on_key.
        modifiers with_key(key which, action act, modifiers reported) const noexcept
        {
            const modifiers bit = modifier_of(which);
            if (bit == modifiers::none)
                return reported;
            if (act != action::release || m_state.is_down(twin(which)))
                return reported | bit;
            return reported & ~bit;
        }

        void press(mouse_button button, modifiers mods, input_time now)
        {
            const math::vec2i at = m_state.cursor();

            int clicks = 1;
            if (m_series.clicks > 0 && m_series.button == button && now - m_series.time <= m_settings.multi_click_time &&
                distance(at, m_series.position) <= m_settings.multi_click_distance)
            {
                clicks = m_series.clicks + 1;
                if (m_settings.max_clicks > 0 && clicks > m_settings.max_clicks)
                    clicks = 1;
            }
            m_series = {button, now, at, clicks};

            m_presses[index(button)] = {at, clicks, false};
            if (!m_state.captured())
            {
                m_drag_button = button;
                m_dragging = false;
            }

            emit(mouse_button_event{button, action::press, at, mods, m_state.buttons() | mask_of(button), clicks, false,
                                    now});
        }

        void release(mouse_button button, modifiers mods, input_time now, bool cancelled)
        {
            const math::vec2i at = m_state.cursor();
            const press_record &record = m_presses[index(button)];

            if (m_drag_button == button && m_dragging)
            {
                m_dragging = false;
                emit(drag_end_event{button, record.origin, at, mods, cancelled, now});
            }
            else if (!cancelled && !record.moved)
            {
                emit(click_event{button, at, record.clicks, mods, now});
            }

            // A press that came to nothing does not start a double click.
            if ((cancelled || record.moved) && m_series.button == button)
                m_series.clicks = 0;
            if (m_drag_button == button)
                m_drag_button.reset();

            emit(mouse_button_event{button, action::release, at, mods, m_state.buttons() & ~mask_of(button),
                                    record.clicks, cancelled, now});

            if (!m_state.captured() && std::exchange(m_leave_pending, false))
                emit(mouse_leave_event{at, m_state.mods(), now});
        }

        void move_to(const math::vec2i &position, input_time now)
        {
            if (position == m_state.cursor())
                return;

            const math::vec2i delta = position - m_state.cursor();
            emit(mouse_move_event{position, delta, m_state.mods(), m_state.buttons(), now});

            for (std::size_t i = 0; i < mouse_button_count; ++i)
            {
                press_record &record = m_presses[i];
                if (has(m_state.buttons(), static_cast<mouse_button>(i)) && !record.moved &&
                    distance(position, record.origin) > m_settings.drag_threshold)
                    record.moved = true;
            }

            if (!m_drag_button)
                return;

            const mouse_button button = *m_drag_button;
            const press_record &record = m_presses[index(button)];
            if (m_dragging)
            {
                emit(drag_move_event{button, record.origin, position, position - m_drag_position, m_state.mods(), now});
                m_drag_position = position;
            }
            else if (record.moved)
            {
                m_dragging = true;
                m_drag_position = position;
                emit(drag_begin_event{button, record.origin, position, m_state.mods(), now});
            }
        }

        void release_all(input_time now)
        {
            const modifiers locks = m_state.mods() & ~held_modifiers;

            // The drag's button last, so the other releases happen while the
            // drag is still on, as they would have had the user let go.
            for (std::size_t i = 0; i < mouse_button_count; ++i)
            {
                const auto button = static_cast<mouse_button>(i);
                if (m_state.is_down(button) && m_drag_button != button)
                    release(button, locks, now, true);
            }
            if (m_drag_button && m_state.is_down(*m_drag_button))
                release(*m_drag_button, locks, now, true);

            for (std::size_t i = 0; i < key_count; ++i)
            {
                const auto k = static_cast<key>(i);
                if (m_state.is_down(k))
                    emit(key_event{k, m_scancodes[i], action::release, locks, true, now});
            }

            m_series.clicks = 0;
        }

        void update_window_state()
        {
            const window_state current = m_minimized   ? window_state::minimized
                                       : m_maximized ? window_state::maximized
                                                     : window_state::normal;
            if (std::exchange(m_window_state, current) != current)
                emit(window_state_event{current, m_now()});
        }

        output m_output;
        gesture_settings m_settings;
        clock m_now;

        input_state m_state;

        // The scancode each held key was pressed with, for its synthetic release.
        std::array<int, key_count> m_scancodes{};

        std::array<press_record, mouse_button_count> m_presses{};
        series m_series;

        // The button that started the capture, which is the one that can drag.
        std::optional<mouse_button> m_drag_button;
        bool m_dragging = false;
        math::vec2i m_drag_position{}; // as of the last drag event

        // The cursor left while captured; announce it on the last release.
        bool m_leave_pending = false;

        bool m_maximized = false;
        bool m_minimized = false;
        window_state m_window_state = window_state::normal;
    };

} // namespace volt::ui
