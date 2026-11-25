#pragma once

#include <ui/controllers.hpp>
#include <ui/widgets/viewport.hpp>

#include <math/vector.hpp>

#include <cstdint>
#include <functional>

// -------------------------------------------------------------------------
// board_view_controller
// -------------------------------------------------------------------------
//
// What the board editor's view does with the pointer (<ui/widgets/
// viewport.hpp>): the orbit camera's navigation (<ui/controllers.hpp>),
// plus clicks and grabs, told apart by how far the pointer moves.
//
//   left press, released within `click_slop` pixels    a click: `clicked`
//   left drag that began on something grabbable         a grab: `grabbed`
//                                                       as it moves,
//                                                       `released` at the
//                                                       end; Escape cancels
//   any other left drag                                 orbits (Shift: pans)
//   middle or right drag, the wheel                     pan, dolly, as ever
//
// A left press is held back from the orbit controller until the pointer has
// moved past the slop, so that a click does not nudge the camera. Whether a
// press can grab is asked at the press (`can_grab`); Shift never grabs.
//
// The controller knows nothing of the board: positions go out in the
// view's pixels and the editor decides what they mean. Interface thread.

namespace volt::ui
{

    class board_view_controller : public viewport_controller
    {
    public:
        explicit board_view_controller(orbit_controller &orbit) noexcept : m_orbit(orbit) {}

        // How far, in the view's pixels, a press may move and still be a
        // click.
        float click_slop = 5.0f;

        // Whether a left press at `pixel` of a view `size` big lands on
        // something a drag would move.
        std::function<bool(const math::vec2f &pixel, const math::vec2f &size)> can_grab;

        // A left click at `pixel`.
        std::function<void(const math::vec2f &pixel, const math::vec2f &size, modifiers mods)> clicked;

        // A grab moved: from where the press was to where the pointer is.
        std::function<void(const math::vec2f &from, const math::vec2f &to, const math::vec2f &size)> grabbed;

        // A grab ended: let go, or cancelled (Escape, or the press taken away).
        std::function<void(bool cancelled)> released;

        bool grabbing() const noexcept { return m_state == state::grabbing; }

        bool pointer_down(const viewport_pointer &e) override;
        bool pointer_up(const viewport_pointer &e) override;
        bool pointer_move(const viewport_pointer &e) override;
        bool scroll(const math::vec2f &notches, const viewport_pointer &e) override;
        bool key(const key_event &e, const math::vec2f &size) override;

    private:
        enum class state : std::uint8_t
        {
            idle,
            pressed,   // the left button is down, not yet a drag
            orbiting,  // a left drag handed to the orbit controller
            grabbing,  // a left drag moving something
            cancelled, // a grab cancelled; the rest of the drag does nothing
        };

        orbit_controller &m_orbit;
        state m_state = state::idle;
        bool m_grabbable = false;
        viewport_pointer m_down;
    };

} // namespace volt::ui
