#include <app/ui/windows/board_view.hpp>

#include <cmath>

namespace volt::ui
{

    bool board_view_controller::pointer_down(const viewport_pointer &e)
    {
        if (e.button != mouse_button::left)
        {
            // Middle and right pan, unless the left button is busy.
            return m_state == state::idle ? m_orbit.pointer_down(e) : false;
        }
        if (m_state != state::idle)
            return false;

        m_state = state::pressed;
        m_down = e;
        m_grabbable = !has(e.mods, modifiers::shift) && can_grab && can_grab(e.position, e.size);
        return false;
    }

    bool board_view_controller::pointer_move(const viewport_pointer &e)
    {
        switch (m_state)
        {
        case state::pressed:
        {
            const math::vec2f moved = e.position - m_down.position;
            if (std::sqrt(moved.x() * moved.x() + moved.y() * moved.y()) <= click_slop)
                return false;
            if (m_grabbable)
            {
                m_state = state::grabbing;
                if (grabbed)
                    grabbed(m_down.position, e.position, e.size);
                return false;
            }

            // The drag the orbit controller missed, all at once.
            m_state = state::orbiting;
            (void)m_orbit.pointer_down(m_down);
            viewport_pointer caught_up = e;
            caught_up.delta = moved;
            return m_orbit.pointer_move(caught_up);
        }
        case state::grabbing:
            if (grabbed)
                grabbed(m_down.position, e.position, e.size);
            return false;
        case state::cancelled:
            return false;
        case state::orbiting:
        case state::idle:
            break;
        }
        return m_orbit.pointer_move(e);
    }

    bool board_view_controller::pointer_up(const viewport_pointer &e)
    {
        if (e.button != mouse_button::left)
            return m_orbit.pointer_up(e);

        const state was = m_state;
        m_state = state::idle;
        switch (was)
        {
        case state::pressed:
            if (clicked)
                clicked(m_down.position, e.size, m_down.mods);
            return false;
        case state::grabbing:
            if (released)
                released(false);
            return false;
        case state::orbiting:
            return m_orbit.pointer_up(e);
        case state::cancelled:
        case state::idle:
            break;
        }
        return false;
    }

    bool board_view_controller::scroll(const math::vec2f &notches, const viewport_pointer &e)
    {
        return m_orbit.scroll(notches, e);
    }

    bool board_view_controller::key(const key_event &e, const math::vec2f &size)
    {
        if (m_state == state::grabbing && e.key == key::escape && e.action == action::press)
        {
            m_state = state::cancelled;
            if (released)
                released(true);
            return true;
        }
        return m_orbit.key(e, size);
    }

} // namespace volt::ui
