#pragma once

#include <ui/widgets/viewport.hpp>

#include <math/vector.hpp>

// -------------------------------------------------------------------------
// view controllers
// -------------------------------------------------------------------------
//
// The two ways the application's views are moved around, as viewport
// controllers (<ui/widgets/viewport.hpp>). They own a camera of their own,
// plain numbers, rather than the renderers' camera types, so that they are
// testable without a GPU; the application copies the camera into its scene
// whenever the viewport says it changed.
//
// pan_zoom_controller, for flat views (the schematic). The camera is the
// world point at the view's centre and a zoom in pixels per world unit;
// world y points up, pixel y down. Dragging with the middle or right button
// pans, so that the point grabbed stays under the pointer; the wheel zooms
// about the pointer, so that the point under it stays there too. Home fits
// the view to fit_area().
//
// orbit_controller, for 3D views (the board). The camera looks at a target
// from a distance, at a yaw (about the z axis, which is up) and a pitch
// (above the xy plane). Dragging with the left button orbits, with the
// middle or right button pans (the target moves in the view's plane, as far
// as the pointer moves at the target's depth), the wheel dollies towards
// the target.

namespace volt::ui
{

    class pan_zoom_controller : public viewport_controller
    {
    public:
        math::vec2f center{0.0f, 0.0f};
        float zoom = 32.0f;
        float min_zoom = 2.0f;
        float max_zoom = 2000.0f;

        // How much one notch of the wheel zooms.
        float zoom_step = 1.15f;

        // A pixel of the view (y down, from its top-left) in world units,
        // and back.
        math::vec2f to_world(const math::vec2f &pixel, const math::vec2f &size) const noexcept;
        math::vec2f to_pixels(const math::vec2f &world, const math::vec2f &size) const noexcept;

        // Zooms by `factor` keeping the world point under `pixel` in place.
        void zoom_about(const math::vec2f &pixel, const math::vec2f &size, float factor) noexcept;

        // What Home fits into view: lower-left and upper-right, world units.
        math::vec2f fit_min{-10.0f, -5.0f};
        math::vec2f fit_max{10.0f, 5.0f};
        void fit(const math::vec2f &size) noexcept;

        bool pointer_down(const viewport_pointer &e) override;
        bool pointer_up(const viewport_pointer &e) override;
        bool pointer_move(const viewport_pointer &e) override;
        bool scroll(const math::vec2f &notches, const viewport_pointer &e) override;
        bool key(const key_event &e, const math::vec2f &size) override;

        bool panning() const noexcept { return m_panning; }

    private:
        bool m_panning = false;
    };

    class orbit_controller : public viewport_controller
    {
    public:
        math::vec3f target{0.0f, 0.0f, 0.0f};
        float distance = 150.0f;
        float yaw = -1.05f;  // radians, about +z, from +x
        float pitch = 0.55f; // radians above the xy plane
        float fov_y = 0.6f;  // to pan by the right amount

        float min_distance = 5.0f;
        float max_distance = 2000.0f;

        // Radians per pixel dragged.
        float orbit_speed = 0.008f;

        math::vec3f eye() const noexcept;

        // Puts the camera at `eye`, looking at `look_at`.
        void look(const math::vec3f &eye, const math::vec3f &look_at) noexcept;

        bool pointer_down(const viewport_pointer &e) override;
        bool pointer_up(const viewport_pointer &e) override;
        bool pointer_move(const viewport_pointer &e) override;
        bool scroll(const math::vec2f &notches, const viewport_pointer &e) override;

    private:
        enum class mode
        {
            none,
            orbit,
            pan,
        };
        mode m_mode = mode::none;
    };

} // namespace volt::ui
