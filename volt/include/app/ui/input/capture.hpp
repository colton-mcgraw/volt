#pragma once

#include <app/ui/input.hpp>
#include <app/ui/input/events.hpp>

#include <concepts>
#include <optional>
#include <type_traits>

// -------------------------------------------------------------------------
// pointer_capture
// -------------------------------------------------------------------------
//
// Routing for pointer events, for whatever the interface uses as a target (a
// widget pointer, an id). The rule: the target under the cursor when the
// first button goes down keeps every pointer event until the last button
// comes up, wherever the cursor goes in between. That is what lets a slider
// follow the mouse past its end and a button notice that the release
// happened elsewhere.
//
// The events are ordered to make this work: a press starts the capture, the
// click_event or drag_end_event a release completes arrives before the
// release itself, and the release that leaves no button held ends the
// capture (see <app/ui/input/events.hpp>).
//
// `hit` is a callable returning the target under the cursor; it is only
// called when nothing is captured, so the hit test is skipped during a drag.
// Scrolling is not routed by capture: the wheel goes to what is under the
// cursor, held button or not.
//
// Used on whichever thread consumes the events; not synchronized.

namespace volt::ui
{

    template <typename Target>
    class pointer_capture
    {
    public:
        // The target of a button event, starting or ending the capture.
        template <typename Hit>
            requires std::convertible_to<std::invoke_result_t<Hit &>, Target>
        Target button(const mouse_button_event &e, Hit &&hit)
        {
            if (!m_target)
            {
                Target under = hit();
                // A release with no capture (one began before this object
                // was watching) goes where the cursor is and captures nothing.
                if (e.action == action::press && e.buttons != mouse_buttons::none)
                    m_target = under;
                return under;
            }

            Target routed = *m_target;
            if (e.buttons == mouse_buttons::none)
                m_target.reset();
            return routed;
        }

        // The target of motion, clicks and drags: the captured one, if any.
        template <typename Hit>
            requires std::convertible_to<std::invoke_result_t<Hit &>, Target>
        Target pointer(Hit &&hit) const
        {
            return m_target ? *m_target : Target(hit());
        }

        const std::optional<Target> &captured() const noexcept { return m_target; }

        // Lets the capture go early, for when its target disappears. The rest
        // of the gesture is then routed by hit test.
        void reset() noexcept { m_target.reset(); }

        // Hands the capture to `target` for the rest of the gesture: for a
        // target that takes over what another began (a container taking a
        // drag from the child that was pressed). Only meaningful while a
        // button is held; the release that leaves none held still ends it.
        void transfer(Target target) noexcept { m_target = std::move(target); }

    private:
        std::optional<Target> m_target;
    };

} // namespace volt::ui
