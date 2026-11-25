#pragma once

#include <app/ui/input.hpp>
#include <app/ui/input/events.hpp>
#include <math/vector.hpp>

#include <bitset>
#include <cstddef>
#include <variant>

// -------------------------------------------------------------------------
// input_state
// -------------------------------------------------------------------------
//
// What is held and where the cursor is, as told by the events applied to it
// so far, and by nothing else. That is what keeps it consistent with the
// events: whoever applies each event before handing it on (the processor on
// the platform thread, the queue's drain on the consuming thread) has a state
// that matches exactly the events its listeners have seen, never one that is
// ahead of them.
//
// Not synchronized: one thread owns each instance.

namespace volt::ui
{

    class input_state
    {
    public:
        bool is_down(key k) const noexcept { return k != key::unknown && m_keys.test(index(k)); }
        bool is_down(mouse_button button) const noexcept { return has(m_buttons, button); }
        bool any_key_down() const noexcept { return m_keys.any(); }

        mouse_buttons buttons() const noexcept { return m_buttons; }
        modifiers mods() const noexcept { return m_mods; }

        // The pointer is captured: a button is held, so motion goes to
        // whatever the first press went to.
        bool captured() const noexcept { return m_buttons != mouse_buttons::none; }

        // The last position reported, inside the window or (while captured)
        // outside it.
        const math::vec2i &cursor() const noexcept { return m_cursor; }

        // The cursor is over the content area, or left it while captured and
        // the leave has not been announced yet (see mouse_leave_event).
        bool cursor_inside() const noexcept { return m_inside; }

        bool focused() const noexcept { return m_focused; }

        // The window's content scale, as of the last window_scale_event: 1
        // until one says otherwise.
        float scale() const noexcept { return m_scale; }

        // ---- applying events ------------------------------------------------

        void apply(const key_event &e) noexcept
        {
            if (e.key != key::unknown)
                m_keys.set(index(e.key), e.action != action::release);
            m_mods = e.mods;
        }

        void apply(const mouse_button_event &e) noexcept
        {
            m_buttons = e.buttons;
            m_cursor = e.position;
            m_mods = e.mods;
        }

        void apply(const mouse_move_event &e) noexcept
        {
            m_cursor = e.position;
            m_buttons = e.buttons;
            m_mods = e.mods;
        }

        void apply(const mouse_enter_event &e) noexcept
        {
            m_cursor = e.position;
            m_inside = true;
        }

        void apply(const mouse_leave_event &e) noexcept
        {
            m_cursor = e.position;
            m_inside = false;
        }

        void apply(const focus_event &e) noexcept { m_focused = e.focused; }
        void apply(const window_scale_event &e) noexcept { m_scale = e.scale; }

        // Everything else changes nothing tracked here.
        template <typename Event>
        void apply(const Event &) noexcept
        {
        }

        void apply(const input_event &event) noexcept
        {
            std::visit([this](const auto &e) { apply(e); }, event);
        }

    private:
        static constexpr std::size_t index(key k) noexcept { return static_cast<std::size_t>(k); }

        std::bitset<key_count> m_keys;
        mouse_buttons m_buttons = mouse_buttons::none;
        modifiers m_mods = modifiers::none;
        math::vec2i m_cursor{};
        bool m_inside = false;
        bool m_focused = false;
        float m_scale = 1.0f;
    };

} // namespace volt::ui
