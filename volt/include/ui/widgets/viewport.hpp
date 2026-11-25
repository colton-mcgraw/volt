#pragma once

#include <ui/draw_list.hpp>
#include <ui/signal.hpp>
#include <ui/widget.hpp>

#include <math/vector.hpp>

#include <string>

// -------------------------------------------------------------------------
// viewport
// -------------------------------------------------------------------------
//
// Shows an image rendered elsewhere, a view's render target, and hands the
// pointer and keyboard input it gets to a controller: the widget that hosts
// the schematic and the board.
//
// The viewport works in the target's pixels, not logical units: its pixel
// size is exactly the size its image is drawn at (its bounds snapped to
// pixels, as the painter snaps them), which is the size to render the
// target at, and controllers get positions in those pixels, from the
// image's top-left corner. So the image is never scaled, and a controller's
// arithmetic matches the renderer's.
//
// Input: every button press captures the pointer for the controller, so a
// drag that leaves the viewport keeps going. The wheel goes to the
// controller. Keys reach it while the viewport has the focus, which a press
// gives it. A right click that did not turn into a drag emits
// `context_requested`, for a context menu.

namespace volt::ui
{

    // A pointer event as a controller sees it, in the viewport's pixels.
    struct viewport_pointer
    {
        math::vec2f position{};
        math::vec2f delta{};
        math::vec2f size{}; // of the viewport
        mouse_button button = mouse_button::left;
        mouse_buttons buttons = mouse_buttons::none;
        modifiers mods = modifiers::none;
        int clicks = 1;
    };

    // What drives a view from the viewport's input. Each returns whether it
    // did something, which the viewport turns into a redraw request and a
    // `changed` signal.
    class viewport_controller
    {
    public:
        virtual ~viewport_controller() = default;

        virtual bool pointer_down(const viewport_pointer &) { return false; }
        virtual bool pointer_up(const viewport_pointer &) { return false; }
        virtual bool pointer_move(const viewport_pointer &) { return false; }

        // `notches` as scroll_input: positive y is away from the user.
        virtual bool scroll(const math::vec2f & /*notches*/, const viewport_pointer &) { return false; }
        virtual bool key(const key_event &, const math::vec2f & /*size*/) { return false; }
    };

    class viewport : public widget
    {
    public:
        explicit viewport(texture_id image = {});

        void set_image(texture_id image);
        texture_id image() const noexcept { return m_image; }

        // Not owned; must outlive the viewport, or be unset first.
        void set_controller(viewport_controller *controller) noexcept { m_controller = controller; }

        // The image's size in pixels as laid out now: what to render at.
        math::vec2i pixel_size() const noexcept { return m_pixels; }

        // A logical point in the viewport's pixels.
        math::vec2f to_pixels(const math::vec2f &point) const noexcept;

        // Whether the pointer is over it, and where, in its pixels.
        bool has_pointer() const noexcept { return m_has_pointer; }
        math::vec2f pointer_pixels() const noexcept { return m_pointer; }

        void set_radius(float radius);

        // A short name shown in a corner of the image ("Schematic").
        void set_caption(std::string caption);

        // The pixel size changed (after layout).
        signal<math::vec2i> resized;

        // The controller changed the view.
        signal<> changed;

        // The pointer moved over the viewport (pixels), or left it.
        signal<math::vec2f> pointer_moved;
        signal<> pointer_left;

        // A right click, at a logical position (for open_context_menu()).
        signal<math::vec2f> context_requested;

        cursor_shape cursor(const math::vec2f &point) const override;

    protected:
        math::vec2f on_measure(const constraints &limits) override;
        void on_arrange(const rect &bounds) override;
        void on_paint(painter &p) override;

        bool on_pointer_down(const pointer_event &e) override;
        bool on_pointer_up(const pointer_event &e) override;
        void on_pointer_move(const pointer_event &e) override;
        void on_pointer_leave() override;
        bool on_click(const pointer_event &e) override;
        bool on_scroll(const scroll_input &e) override;
        bool on_key(const key_event &e) override;
        void on_focus_changed(bool, focus_reason) override { invalidate(); }

    private:
        viewport_pointer translate(const pointer_event &e) const;
        void report(bool did);

        texture_id m_image;
        viewport_controller *m_controller = nullptr;
        math::vec2i m_pixels{};
        float m_radius = 0.0f;
        std::string m_caption;
        bool m_has_pointer = false;
        math::vec2f m_pointer{};
        bool m_dragging = false;
    };

} // namespace volt::ui
