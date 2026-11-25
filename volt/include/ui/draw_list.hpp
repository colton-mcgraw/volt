#pragma once

#include <ui/geometry.hpp>

#include <math/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// -------------------------------------------------------------------------
// draw_list
// -------------------------------------------------------------------------
//
// What the interface draws in one frame, back to front: every shape a
// desktop interface is made of, in one ordered list. Whoever lays the
// interface out fills it (<ui/painter.hpp>); the renderer of the window's
// pass (<app/ui/renderer.hpp>) only turns it into commands, batching
// neighbouring items that share a clip rectangle and textures.
//
// Everything is in pixels of the window's image, y down. Colours are in the
// working space of <gpu/color.hpp> (linear light, sRGB primaries, 1.0 = SDR
// white) with alpha not multiplied in; the renderer premultiplies, and
// encodes for the display.
//
// Textures are named by texture_id, never by a GPU handle, so that the list,
// and everything that fills it, builds and is tested without Vulkan. The
// renderer resolves the ids: glyph pages are the pages of the text engine's
// atlas, images are whatever the application registered under that index
// (an icon, a view's render target).
//
// Clipping: push_clip() narrows the region items are drawn in to its
// intersection with the current one, pop_clip() goes back. Items record the
// clip they were added under; items entirely outside it are not added at
// all.

namespace volt::ui
{

    enum class texture_kind : std::uint8_t
    {
        none,
        image,  // an image registered with the renderer: rgba, or a view's target
        glyphs, // a page of the text engine's glyph atlas: coverage
    };

    struct texture_id
    {
        texture_kind kind = texture_kind::none;
        std::uint32_t index = 0;

        static constexpr texture_id image(std::uint32_t index) noexcept { return {texture_kind::image, index}; }
        static constexpr texture_id glyph_page(std::uint32_t page) noexcept { return {texture_kind::glyphs, page}; }

        constexpr bool valid() const noexcept { return kind != texture_kind::none; }
        friend constexpr bool operator==(const texture_id &, const texture_id &) = default;
    };

    enum class shape_kind : std::uint8_t
    {
        // A rectangle with rounded corners, filled, optionally with a border
        // drawn inside its edge.
        rectangle,

        // The soft shadow of a rounded rectangle, blurred by `extra` pixels.
        shadow,

        // A straight line with round ends from one point to another,
        // `extra` pixels wide, antialiased.
        line,

        // A glyph from the atlas: its coverage, in `color`.
        glyph,

        // An image, multiplied by `color`, optionally with rounded corners.
        image,
    };

    struct draw_item
    {
        shape_kind kind = shape_kind::rectangle;

        // For a line its two ends; otherwise the rectangle the shape fills
        // (a shadow spills past it by its blur).
        rect bounds;
        math::vec2f from{};
        math::vec2f to{};

        math::vec4f color{1.0f, 1.0f, 1.0f, 1.0f};
        math::vec4f border_color{0.0f, 0.0f, 0.0f, 0.0f};

        // The part of the texture to show, in texture coordinates 0 to 1.
        math::vec2f uv_min{0.0f, 0.0f};
        math::vec2f uv_max{1.0f, 1.0f};

        float radius = 0.0f;
        float border = 0.0f; // width of the border, pixels; 0 for none

        // Per kind: line width, shadow blur, glyph contrast (see glyph()).
        float extra = 0.0f;

        texture_id texture;

        // Index into clips().
        std::uint32_t clip = 0;
    };

    class draw_list
    {
    public:
        draw_list() { clear(); }

        // Empties the list, the clip stack included.
        void clear();

        // ---- shapes ----------------------------------------------------------

        void rectangle(const rect &area, const math::vec4f &color, float radius = 0.0f);

        // Filled with `fill`, with a border `border` pixels wide inside its
        // edge in `border_color`. A transparent fill gives an outline.
        void rectangle(const rect &area, const math::vec4f &fill, float radius, float border,
                       const math::vec4f &border_color);

        void shadow(const rect &area, float radius, float blur, const math::vec4f &color);

        void line(const math::vec2f &from, const math::vec2f &to, float width, const math::vec4f &color);

        // Lines through every point in turn. Joins are round, as the ends
        // are; with a translucent colour the joins overlap and show darker.
        void polyline(std::span<const math::vec2f> points, float width, const math::vec4f &color);

        // `texture` with its uv_min to uv_max part stretched over `area`.
        void image(const rect &area, texture_id texture, const math::vec4f &tint = {1.0f, 1.0f, 1.0f, 1.0f},
                   float radius = 0.0f, const math::vec2f &uv_min = {0.0f, 0.0f},
                   const math::vec2f &uv_max = {1.0f, 1.0f});

        // One glyph from a page of the glyph atlas. `contrast` above 0 makes
        // the coverage heavier (the shader raises it to 1 / (1 + contrast)),
        // for text that blending leaves looking thin.
        void glyph(const rect &area, texture_id page, const math::vec2f &uv_min, const math::vec2f &uv_max,
                   const math::vec4f &color, float contrast = 0.0f);

        // ---- clipping --------------------------------------------------------

        void push_clip(const rect &area);
        void pop_clip();

        // The region items are drawn in now: the intersection of every clip
        // pushed, or everything when none is.
        const rect &current_clip() const noexcept { return m_clips[m_stack.back()]; }
        std::size_t clip_depth() const noexcept { return m_stack.size() - 1; }

        // ---- contents ----------------------------------------------------------

        std::span<const draw_item> items() const noexcept { return m_items; }
        std::span<const rect> clips() const noexcept { return m_clips; }
        bool empty() const noexcept { return m_items.empty(); }

        // The clip 0 stands for: larger than any window.
        static constexpr rect no_clip{-1.0e7f, -1.0e7f, 2.0e7f, 2.0e7f};

    private:
        // Adds `item` under the current clip, unless `extent` (what it
        // touches) lies outside it.
        void add(draw_item item, const rect &extent);

        std::vector<draw_item> m_items;
        std::vector<rect> m_clips;
        std::vector<std::uint32_t> m_stack;
    };

} // namespace volt::ui
