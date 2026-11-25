#pragma once

#include <app/ui/windows/schematic_tools.hpp>
#include <ui/controllers.hpp>
#include <ui/widgets/viewport.hpp>

#include <math/vector.hpp>

#include <functional>

// -------------------------------------------------------------------------
// schematic_view_controller
// -------------------------------------------------------------------------
//
// What the schematic editor's view does with the pointer and the keys
// (<ui/widgets/viewport.hpp>): the editing tools (<app/ui/windows/
// schematic_tools.hpp>) first, then the camera (pan_zoom_controller,
// <ui/controllers.hpp>), as the tools' own manual says an adapter must:
//
//   every press, move and release   to the tools, in sheet units
//                                   (to_world() of the pixel, times 100),
//                                   with a tolerance of `pick_distance`
//                                   logical units at the view's zoom; what
//                                   they do not consume goes on to the
//                                   camera: the middle and right buttons
//                                   pan, unless the tools took the right
//                                   press (ending a wire, cancelling a drag)
//   the wheel                       the camera's: zooms about the pointer
//   a key, while the view has the   to the tools (Escape, Enter, Delete,
//   focus                           R, X, Y, Space, the arrows, ...), then
//                                   the camera (Home, =, -); unconsumed, the
//                                   window's shortcuts see it
//
// A right click opens the context menu (the viewport's context_requested)
// unless the tools took its press: context_menu_allowed() says which, and
// holds from the press until the next one (the click comes before the
// release). A right drag that pans is no click at all.
//
// tools_changed is called after any event that may have changed what the
// tools show (the selection, the overlay, the hint, the document): the
// editor then pushes the selection and the overlay into the scene.
//
// schematic_viewport is the view itself: a viewport whose pointer shape is
// the tools' (an arrow, a crosshair while drawing or placing, a move over
// what a drag would move), or the move shape while the camera pans.
//
// Interface thread.

namespace volt::ui
{

    class schematic_view_controller : public viewport_controller
    {
    public:
        schematic_view_controller(schematic_tools &tools, pan_zoom_controller &camera) noexcept
            : m_tools(tools), m_camera(camera)
        {
        }

        // How near the pointer must be to pick something, and how far it
        // must move to make a drag, in logical units; and the view's pixels
        // per logical unit (the window's scale), which the editor keeps up
        // to date.
        float pick_distance = 5.0f;
        float pixel_scale = 1.0f;

        // A pixel of a view `size` big in sheet units, and the tolerance in
        // sheet units at the camera's zoom.
        math::vec2f to_sheet(const math::vec2f &pixel, const math::vec2f &size) const noexcept;
        float tolerance() const noexcept;

        // After an event that changed what the tools show.
        std::function<void()> tools_changed;

        // The pointer left the view (the viewport's pointer_left).
        void leave();

        // Whether the right click now being reported should open the
        // context menu.
        bool context_menu_allowed() const noexcept { return !m_right_taken; }

        // The camera is being dragged.
        bool panning() const noexcept { return m_camera.panning(); }

        // The pointer shape for the view.
        cursor_shape cursor() const noexcept;

        bool pointer_down(const viewport_pointer &e) override;
        bool pointer_up(const viewport_pointer &e) override;
        bool pointer_move(const viewport_pointer &e) override;
        bool scroll(const math::vec2f &notches, const viewport_pointer &e) override;
        bool key(const key_event &e, const math::vec2f &size) override;

    private:
        schematic_tools::pointer_input input(const viewport_pointer &e) const noexcept;
        // Tells the editor, and says whether the viewport should redraw.
        bool report(const schematic_tools::input_result &r);

        schematic_tools &m_tools;
        pan_zoom_controller &m_camera;
        bool m_right_taken = false;
    };

    class schematic_viewport final : public viewport
    {
    public:
        schematic_viewport(texture_id image, const schematic_view_controller &controller)
            : viewport(image), m_view_controller(controller)
        {
        }

        cursor_shape cursor(const math::vec2f &) const override { return m_view_controller.cursor(); }

    private:
        const schematic_view_controller &m_view_controller;
    };

} // namespace volt::ui
