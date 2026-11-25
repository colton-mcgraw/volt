#pragma once

#include <ui/draw_list.hpp>
#include <ui/painter.hpp>
#include <ui/widget.hpp>

// -------------------------------------------------------------------------
// image_view
// -------------------------------------------------------------------------
//
// An image at a size of its choosing: a texture the renderer knows by id
// (an icon or a logo loaded from a file), or one of the built-in vector
// icons. Lets the pointer through.
//
// Or, fitted (set_fit()), an image as an image viewer shows it: as large as
// the widget's bounds allow with its aspect ratio kept, centred, on a
// checkerboard where it is transparent. The widget then measures as its
// size and is meant to be given more (flex); the image is never drawn
// larger than its own pixels unless asked.

namespace volt::ui
{

    class image_view : public widget
    {
    public:
        image_view(texture_id image, math::vec2f size) : m_image(image), m_size(size) { set_hit_testable(false); }
        image_view(ui::icon icon, float size) : m_icon(icon), m_size{size, size} { set_hit_testable(false); }

        void set_image(texture_id image)
        {
            m_image = image;
            invalidate();
        }

        // Multiplies the image; for an icon, its colour. Unset: white for an
        // image, the theme's text colour for an icon.
        void set_tint(const math::vec4f &tint)
        {
            m_tint = tint;
            m_tinted = true;
            invalidate();
        }

        void set_radius(float radius)
        {
            m_radius = radius;
            invalidate();
        }

        // Fits an image of `pixels` (its width and height in pixels) into
        // the bounds; see above. `enlarge` lets a small image grow past its
        // own size to fill them.
        void set_fit(math::vec2f pixels, bool enlarge = false)
        {
            m_pixels = pixels;
            m_fit = true;
            m_enlarge = enlarge;
            invalidate();
        }

        // Squares behind the image, where its transparency shows them.
        void set_checkerboard(bool on)
        {
            m_checkerboard = on;
            invalidate();
        }

        // Where a fitted image goes in `area` at `scale` pixels per logical
        // unit, logical units, snapped to whole pixels.
        rect fitted(const rect &area, float scale) const noexcept;

    protected:
        math::vec2f on_measure(const constraints &) override { return m_size; }
        void on_paint(painter &p) override;

    private:
        texture_id m_image;
        ui::icon m_icon = icon::none;
        math::vec2f m_size;
        math::vec4f m_tint{1.0f, 1.0f, 1.0f, 1.0f};
        bool m_tinted = false;
        float m_radius = 0.0f;

        math::vec2f m_pixels{};
        bool m_fit = false;
        bool m_enlarge = false;
        bool m_checkerboard = false;
    };

} // namespace volt::ui
