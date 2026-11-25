#include <app/ui/windows/schematic_view.hpp>

#include <schematic/sheet.hpp>

#include <algorithm>

namespace volt::ui
{

    math::vec2f schematic_view_controller::to_sheet(const math::vec2f &pixel, const math::vec2f &size) const noexcept
    {
        return m_camera.to_world(pixel, size) * static_cast<float>(schematic::units_per_grid);
    }

    float schematic_view_controller::tolerance() const noexcept
    {
        const float zoom = std::max(m_camera.zoom, 1.0e-3f);
        return pick_distance * pixel_scale / zoom * static_cast<float>(schematic::units_per_grid);
    }

    schematic_tools::pointer_input schematic_view_controller::input(const viewport_pointer &e) const noexcept
    {
        schematic_tools::pointer_input in;
        in.at = to_sheet(e.position, e.size);
        in.tolerance = tolerance();
        in.button = e.button;
        in.mods = e.mods;
        in.clicks = e.clicks;
        return in;
    }

    bool schematic_view_controller::report(const schematic_tools::input_result &r)
    {
        if (r.changed && tools_changed)
            tools_changed();
        return r.changed;
    }

    void schematic_view_controller::leave()
    {
        (void)report(m_tools.pointer_leave());
    }

    cursor_shape schematic_view_controller::cursor() const noexcept
    {
        if (m_camera.panning())
            return cursor_shape::move;
        switch (m_tools.cursor())
        {
        case schematic_tools::pointer_shape::crosshair:
            return cursor_shape::crosshair;
        case schematic_tools::pointer_shape::move:
            return cursor_shape::move;
        case schematic_tools::pointer_shape::arrow:
            break;
        }
        return cursor_shape::arrow;
    }

    // The tools' changes reach the scene through tools_changed (the editor
    // pushes them into the document, which asks for the redraw); what these
    // return is whether the camera moved, which the viewport passes on as
    // its `changed`. Keys return whether they were used, so that a key the
    // tools took runs no shortcut as well.

    bool schematic_view_controller::pointer_down(const viewport_pointer &e)
    {
        const schematic_tools::input_result r = m_tools.pointer_down(input(e));
        if (e.button == mouse_button::right)
            m_right_taken = r.consumed;
        (void)report(r);
        return !r.consumed && m_camera.pointer_down(e);
    }

    bool schematic_view_controller::pointer_move(const viewport_pointer &e)
    {
        const schematic_tools::input_result r = m_tools.pointer_move(input(e));
        (void)report(r);
        return !r.consumed && m_camera.pointer_move(e);
    }

    bool schematic_view_controller::pointer_up(const viewport_pointer &e)
    {
        const schematic_tools::input_result r = m_tools.pointer_up(input(e));
        (void)report(r);
        return !r.consumed && m_camera.pointer_up(e);
    }

    bool schematic_view_controller::scroll(const math::vec2f &notches, const viewport_pointer &e)
    {
        return m_camera.scroll(notches, e);
    }

    bool schematic_view_controller::key(const key_event &e, const math::vec2f &size)
    {
        const schematic_tools::input_result r = m_tools.key_down(e.key, e.mods);
        (void)report(r);
        if (r.consumed)
            return true;
        return m_camera.key(e, size);
    }

} // namespace volt::ui
