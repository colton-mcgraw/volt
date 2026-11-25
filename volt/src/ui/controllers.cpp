#include <ui/controllers.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace volt::ui
{

    // ---- pan_zoom_controller ------------------------------------------------------

    math::vec2f pan_zoom_controller::to_world(const math::vec2f &pixel, const math::vec2f &size) const noexcept
    {
        const math::vec2f from_center = pixel - size * 0.5f;
        return {center.x() + from_center.x() / zoom, center.y() - from_center.y() / zoom};
    }

    math::vec2f pan_zoom_controller::to_pixels(const math::vec2f &world, const math::vec2f &size) const noexcept
    {
        return {(world.x() - center.x()) * zoom + size.x() * 0.5f, -(world.y() - center.y()) * zoom + size.y() * 0.5f};
    }

    void pan_zoom_controller::zoom_about(const math::vec2f &pixel, const math::vec2f &size, float factor) noexcept
    {
        const math::vec2f anchor = to_world(pixel, size);
        zoom = std::clamp(zoom * factor, min_zoom, max_zoom);

        // Move the centre so that the anchor is back under the pixel.
        const math::vec2f from_center = pixel - size * 0.5f;
        center = {anchor.x() - from_center.x() / zoom, anchor.y() + from_center.y() / zoom};
    }

    void pan_zoom_controller::fit(const math::vec2f &size) noexcept
    {
        const math::vec2f extent = fit_max - fit_min;
        center = (fit_min + fit_max) * 0.5f;
        if (extent.x() > 0.0f && extent.y() > 0.0f && size.x() > 0.0f && size.y() > 0.0f)
            zoom = std::clamp(0.9f * std::min(size.x() / extent.x(), size.y() / extent.y()), min_zoom, max_zoom);
    }

    bool pan_zoom_controller::pointer_down(const viewport_pointer &e)
    {
        if (e.button == mouse_button::middle || e.button == mouse_button::right)
            m_panning = true;
        return false;
    }

    bool pan_zoom_controller::pointer_up(const viewport_pointer &e)
    {
        if (!has(e.buttons, mouse_button::middle) && !has(e.buttons, mouse_button::right))
            m_panning = false;
        return false;
    }

    bool pan_zoom_controller::pointer_move(const viewport_pointer &e)
    {
        if (!m_panning || (e.delta.x() == 0.0f && e.delta.y() == 0.0f))
            return false;
        // The world moves with the pointer: the centre against it.
        center = {center.x() - e.delta.x() / zoom, center.y() + e.delta.y() / zoom};
        return true;
    }

    bool pan_zoom_controller::scroll(const math::vec2f &notches, const viewport_pointer &e)
    {
        if (notches.y() == 0.0f)
            return false;
        const float before = zoom;
        zoom_about(e.position, e.size, std::pow(zoom_step, notches.y()));
        return zoom != before;
    }

    bool pan_zoom_controller::key(const key_event &e, const math::vec2f &size)
    {
        if ((e.mods & held_modifiers) != modifiers::none)
            return false;
        switch (e.key)
        {
        case key::home:
            fit(size);
            return true;
        case key::equal:
        case key::kp_add:
            zoom_about(size * 0.5f, size, zoom_step);
            return true;
        case key::minus:
        case key::kp_subtract:
            zoom_about(size * 0.5f, size, 1.0f / zoom_step);
            return true;
        default:
            return false;
        }
    }

    // ---- orbit_controller ---------------------------------------------------------

    math::vec3f orbit_controller::eye() const noexcept
    {
        const float c = std::cos(pitch);
        return target + math::vec3f{c * std::cos(yaw), c * std::sin(yaw), std::sin(pitch)} * distance;
    }

    void orbit_controller::look(const math::vec3f &from, const math::vec3f &look_at) noexcept
    {
        target = look_at;
        const math::vec3f d = from - look_at;
        distance = std::clamp(std::sqrt(d.x() * d.x() + d.y() * d.y() + d.z() * d.z()), min_distance, max_distance);
        yaw = std::atan2(d.y(), d.x());
        pitch = std::asin(std::clamp(d.z() / std::max(distance, 1e-6f), -1.0f, 1.0f));
    }

    bool orbit_controller::pointer_down(const viewport_pointer &e)
    {
        if (m_mode != mode::none)
            return false;
        if (e.button == mouse_button::left)
            m_mode = has(e.mods, modifiers::shift) ? mode::pan : mode::orbit;
        else if (e.button == mouse_button::middle || e.button == mouse_button::right)
            m_mode = mode::pan;
        return false;
    }

    bool orbit_controller::pointer_up(const viewport_pointer &e)
    {
        if (e.buttons == mouse_buttons::none)
            m_mode = mode::none;
        return false;
    }

    bool orbit_controller::pointer_move(const viewport_pointer &e)
    {
        if (m_mode == mode::none || (e.delta.x() == 0.0f && e.delta.y() == 0.0f))
            return false;

        if (m_mode == mode::orbit)
        {
            // Dragging right turns the scene right: the eye goes left.
            yaw -= e.delta.x() * orbit_speed;
            const float limit = 0.5f * std::numbers::pi_v<float> - 0.01f;
            pitch = std::clamp(pitch + e.delta.y() * orbit_speed, -limit, limit);
            return true;
        }

        // Pan: one pixel is this far at the target's depth.
        const float height = std::max(1.0f, e.size.y());
        const float per_pixel = 2.0f * distance * std::tan(fov_y * 0.5f) / height;

        // The view's right and up, from yaw and pitch (z is up).
        const math::vec3f right{-std::sin(yaw), std::cos(yaw), 0.0f};
        const math::vec3f up{-std::sin(pitch) * std::cos(yaw), -std::sin(pitch) * std::sin(yaw), std::cos(pitch)};
        target = target - right * (e.delta.x() * per_pixel) + up * (e.delta.y() * per_pixel);
        return true;
    }

    bool orbit_controller::scroll(const math::vec2f &notches, const viewport_pointer &)
    {
        if (notches.y() == 0.0f)
            return false;
        const float before = distance;
        distance = std::clamp(distance * std::pow(0.88f, notches.y()), min_distance, max_distance);
        return distance != before;
    }

} // namespace volt::ui
