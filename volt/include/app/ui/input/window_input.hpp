#pragma once

#include <app/ui/input.hpp>
#include <app/ui/input/events.hpp>
#include <app/ui/input/processor.hpp>
#include <app/ui/input/state.hpp>
#include <events/bus.hpp>
#include <math/vector.hpp>

#include <cstddef>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// -------------------------------------------------------------------------
// window_input
// -------------------------------------------------------------------------
//
// Connects a window to the event bus: it installs every one of the window's
// callbacks, runs them through an input_processor, and publishes the events
// that come out.
//
// Threading. The callbacks run on the platform thread, inside
// platform::poll_events(); the interface is drawn on the render thread. So
// each event is published twice over, to two buses:
//
//   * immediate(): dispatched on the platform thread, synchronously, as the
//     callback that caused the event runs. This is for what has to happen
//     there, because it needs the window (which is platform-thread only) or
//     must not wait for a frame: closing, resizing the swapchain, looking up
//     the display after a move. It is the replacement for setting the
//     window's callbacks directly, which this class has taken over.
//
//   * queued(): the same events, in the same order, stored until the
//     consuming thread calls drain(), which dispatches them there. This is
//     for the interface, which lives on the render thread and wants to see a
//     frame's worth of input at the start of the frame, on its own thread,
//     with no locking of its own. Queueing is off until enable_queue(): with
//     nobody draining, the queue would only grow.
//
// Both are ordinary events::bus objects (threading::multi), so listeners may
// be added and removed from any thread. A listener that handles an event on
// the immediate bus stops only the other immediate listeners; the event is
// queued all the same, because the queue's side has to see every press with
// its release.
//
// Each side has its own input_state, applied event by event just before the
// event is dispatched: state() on the platform thread, queued_state() on the
// consuming thread. Each matches exactly what that side's listeners have
// seen; the consuming side is simply behind by the events not yet drained.
//
// Consecutive motion (mouse_move_event, drag_move_event) is merged in the
// queue while it waits, deltas summed, so a frame sees one move rather than
// the dozens a fast mouse reports. The immediate bus sees every one.

namespace volt::ui
{

    class window_input
    {
    public:
        explicit window_input(gesture_settings settings = {}, input_processor::clock now = {})
            : m_processor([this](input_event &&e) { publish(std::move(e)); }, settings, std::move(now))
        {
        }

        // The window's callbacks point at this object.
        window_input(const window_input &) = delete;
        window_input &operator=(const window_input &) = delete;

        ~window_input() { detach(); }

        // ---- platform thread ----------------------------------------------

        // Installs this object as every callback of `window`, replacing what
        // was there, and detaches from the previous window if any. `window`
        // is a volt::ui::window, or anything with the same setters and with
        // is_focused() and content_scale() (tests use a stand-in), and must
        // stay alive until detach() or the destructor.
        template <typename Window>
        void attach(Window &window)
        {
            detach();

            input_processor &p = m_processor;
            window.set_key_callback([&p](key k, int scancode, action a, modifiers m) { p.on_key(k, scancode, a, m); });
            window.set_mouse_callback([&p](mouse_button b, action a, modifiers m) { p.on_mouse(b, a, m); });
            window.set_cursor_callback([&p](const math::vec2i &position) { p.on_cursor(position); });
            window.set_enter_callback([&p](const math::vec2i &position) { p.on_enter(position); });
            window.set_leave_callback([&p](const math::vec2i &position) { p.on_leave(position); });
            window.set_scroll_callback([&p](double x, double y) { p.on_scroll(x, y); });
            window.set_char_callback([&p](char32_t codepoint) { p.on_char(codepoint); });
            window.set_drop_callback([&p](std::span<const std::string> paths) { p.on_drop(paths); });
            window.set_resize_callback([&p](const math::vec2i &size) { p.on_resize(size); });
            window.set_move_callback([&p](const math::vec2i &position) { p.on_move(position); });
            window.set_scale_callback([&p](float scale) { p.on_scale(scale); });
            window.set_focus_callback([&p](bool focused) { p.on_focus(focused); });
            window.set_maximize_callback([&p](bool maximized) { p.on_maximize(maximized); });
            window.set_minimize_callback([&p](bool minimized) { p.on_minimize(minimized); });
            window.set_restore_callback([&p] { p.on_restore(); });
            window.set_close_callback([&p] { p.on_close(); });
            window.set_capture_lost_callback([&p] { p.on_capture_lost(); });

            m_detach = [&window] {
                window.set_key_callback({});
                window.set_mouse_callback({});
                window.set_cursor_callback({});
                window.set_enter_callback({});
                window.set_leave_callback({});
                window.set_scroll_callback({});
                window.set_char_callback({});
                window.set_drop_callback({});
                window.set_resize_callback({});
                window.set_move_callback({});
                window.set_scale_callback({});
                window.set_focus_callback({});
                window.set_maximize_callback({});
                window.set_minimize_callback({});
                window.set_restore_callback({});
                window.set_close_callback({});
                window.set_capture_lost_callback({});
            };

            // A window says what it is only when that changes, so what it
            // already is has to be reported here: its scale (unless it is
            // the one the state has, which is how it starts at 1), so that
            // the interface lays out at the right one from the start, and
            // the keyboard if it has it.
            m_processor.on_scale(window.content_scale());
            if (window.is_focused())
                m_processor.on_focus(true);
        }

        // Removes the callbacks and releases whatever is held, since those
        // releases will not come now. Safe to call from inside a callback.
        void detach()
        {
            if (!m_detach)
                return;
            std::exchange(m_detach, {})();
            m_processor.release_all();
        }

        // Starts queueing for drain(). `wake` is called on the platform thread
        // after each event is queued, to rouse the consuming thread if it
        // sleeps until there is something to do; it must be cheap. Call before
        // attach(), so that the queue sees every event from the start.
        void enable_queue(std::function<void()> wake = {})
        {
            m_wake = std::move(wake);
            m_queueing = true;
        }

        // Synchronous listeners on this bus run on the platform thread.
        events::bus &immediate() noexcept { return m_immediate; }

        // The platform side's state, as of the last event published.
        const input_state &state() const noexcept { return m_processor.state(); }

        // The raw entry points, for feeding input that does not come from the
        // window (tests, scripted input). Platform thread only, like the window.
        input_processor &processor() noexcept { return m_processor; }

        // ---- consuming thread ----------------------------------------------

        // Synchronous listeners on this bus run in drain().
        events::bus &queued() noexcept { return m_queued; }

        // Dispatches, on the calling thread, every event queued since the last
        // drain, and returns how many there were. One thread drains; not from
        // inside a listener of the queued bus, which would reorder events.
        std::size_t drain()
        {
            std::vector<input_event> batch;
            {
                std::lock_guard lock(m_mutex);
                batch.swap(m_pending);
            }

            for (const input_event &event : batch)
            {
                m_queued_state.apply(event);
                std::visit([this](const auto &e) { m_queued.dispatch(e); }, event);
            }
            return batch.size();
        }

        // The consuming side's state, as of the last event drained.
        const input_state &queued_state() const noexcept { return m_queued_state; }

    private:
        // Platform thread: every event the processor puts out.
        void publish(input_event &&event)
        {
            std::visit([this](const auto &e) { m_immediate.dispatch(e); }, event);

            if (!m_queueing)
                return;
            {
                std::lock_guard lock(m_mutex);
                if (!merge(event))
                    m_pending.push_back(std::move(event));
            }
            if (m_wake)
                m_wake();
        }

        // Folds a motion event into one of its kind already waiting, if only
        // motion has been queued since. Moves and drag moves interleave during
        // a drag; each is merged with its own kind, which keeps their order.
        bool merge(const input_event &event)
        {
            for (auto it = m_pending.rbegin(); it != m_pending.rend(); ++it)
            {
                if (const auto *next = std::get_if<mouse_move_event>(&event))
                {
                    if (auto *waiting = std::get_if<mouse_move_event>(&*it))
                    {
                        if (waiting->buttons != next->buttons || waiting->mods != next->mods)
                            return false;
                        const math::vec2i delta = waiting->delta + next->delta;
                        *waiting = *next;
                        waiting->delta = delta;
                        return true;
                    }
                }
                else if (const auto *drag = std::get_if<drag_move_event>(&event))
                {
                    if (auto *waiting = std::get_if<drag_move_event>(&*it))
                    {
                        if (waiting->button != drag->button || waiting->mods != drag->mods)
                            return false;
                        const math::vec2i delta = waiting->delta + drag->delta;
                        *waiting = *drag;
                        waiting->delta = delta;
                        return true;
                    }
                }
                else
                {
                    return false;
                }

                if (!std::holds_alternative<mouse_move_event>(*it) && !std::holds_alternative<drag_move_event>(*it))
                    return false;
            }
            return false;
        }

        input_processor m_processor;
        events::bus m_immediate;
        std::function<void()> m_detach;

        // Platform thread, set before attach().
        bool m_queueing = false;
        std::function<void()> m_wake;

        // Written by the platform thread, swapped out by the consuming one.
        std::mutex m_mutex;
        std::vector<input_event> m_pending;

        // Consuming thread.
        events::bus m_queued;
        input_state m_queued_state;
    };

} // namespace volt::ui
