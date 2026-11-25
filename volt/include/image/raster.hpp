#pragma once

#include <image/image.hpp>
#include <image/path.hpp>

#include <math/matrix.hpp>
#include <math/vector.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// raster
// -------------------------------------------------------------------------
//
// Filling polygons into pixels, antialiased by exact area coverage: every
// edge adds the signed area it sweeps to an accumulation buffer, and a
// running sum along each row gives how much of each pixel is inside (the
// approach of font rasterizers such as font-rs and stb_truetype). Fill
// rules apply to that sum: nonzero takes its magnitude, capped at 1;
// even-odd folds it back from 2. Strokes arrive here as polygons from
// image::stroke().
//
// Colors are given as SVG and CSS give them: straight alpha, sRGB-encoded,
// 0 to 1, as math::vec4f {r, g, b, a}. By default a canvas also blends and
// interpolates in those encoded values, which is what browsers do and what
// makes an SVG look as its author saw it. A canvas made with linear_light
// works in linear light instead (the renderer's working space; edges and
// gradients come out differently, arguably more correct, and not as
// browsers show them).

namespace volt::image
{

    struct gradient_stop
    {
        float offset = 0.0f;              // 0 to 1, non-decreasing along the list
        math::vec4f color{0, 0, 0, 1}; // straight alpha, sRGB-encoded
    };

    enum class spread_method : std::uint8_t
    {
        pad,     // the end colors continue outward
        reflect, // the gradient repeats, mirrored every other time
        repeat,  // the gradient repeats
    };

    struct gradient
    {
        enum class kind : std::uint8_t
        {
            linear,
            radial,
        };

        kind type = kind::linear;

        // Linear: offset 0 at `start`, 1 at `end`, constant along lines
        // perpendicular to the two.
        //
        // Radial: offset 1 on the circle of `radius` around `end` (the
        // center), offset 0 on the circle of `focal_radius` around `start`
        // (the focal point), and the circles in between interpolated: SVG 2's
        // two-point conical gradient, of which SVG 1.1's focal point is the
        // case focal_radius = 0.
        math::vec2f start{0, 0};
        math::vec2f end{1, 0};
        float radius = 0.5f;
        float focal_radius = 0.0f;

        // From the coordinates above to those of the geometry it paints.
        math::mat3f transform = math::mat3f::identity();

        spread_method spread = spread_method::pad;
        std::vector<gradient_stop> stops;
    };

    struct paint
    {
        enum class kind : std::uint8_t
        {
            none,
            color,
            gradient,
        };

        kind type = kind::none;
        math::vec4f color{0, 0, 0, 1}; // kind::color: straight alpha, sRGB-encoded

        // kind::gradient. Shared, since one definition usually paints many
        // shapes.
        std::shared_ptr<const volt::image::gradient> gradient;

        static paint solid(math::vec4f color)
        {
            paint result;
            result.type = kind::color;
            result.color = color;
            return result;
        }

        static paint with(std::shared_ptr<const volt::image::gradient> gradient)
        {
            paint result;
            result.type = kind::gradient;
            result.gradient = std::move(gradient);
            return result;
        }
    };

    // ---------------------------------------------------------------------
    // canvas
    // ---------------------------------------------------------------------
    //
    // Premultiplied RGBA in floats, 16 bytes a pixel, plus one more such
    // buffer per open layer. Pixel coordinates have x to the right and y
    // down; pixel (i, j) is the square from (i, j) to (i + 1, j + 1).
    class canvas
    {
    public:
        // At 16 bytes a pixel, 2^25 pixels (5792 x 5792) is 512 MiB.
        static constexpr std::uint64_t max_pixels = std::uint64_t{1} << 25;

        // Layers open at once. Groups nested deeper than this are drawn
        // straight into the deepest layer, without their own opacity, which
        // keeps a hostile document from asking for a canvas-sized buffer per
        // level of nesting.
        static constexpr std::size_t max_layers = 8;

        // Transparent black. Fails for sizes check_size() rejects, and for
        // more than max_pixels.
        static std::expected<canvas, std::string> create(std::uint32_t width, std::uint32_t height,
                                                         bool linear_light = false);

        std::uint32_t width() const noexcept { return m_width; }
        std::uint32_t height() const noexcept { return m_height; }

        // Paints the area inside the polygons (each implicitly closed), given
        // in pixel coordinates, with `paint` scaled by `opacity`.
        // `paint_transform` maps the paint's coordinates (those of its
        // gradient) to pixels; colors ignore it.
        void fill(std::span<const contour> polygons, fill_rule rule, const paint &paint,
                  const math::mat3f &paint_transform, float opacity = 1.0f);

        // What is drawn after push_layer() goes onto a transparent layer of
        // its own, composited onto what is below with `opacity` by the
        // matching pop_layer(): group opacity, as SVG has it. A pop without
        // a push does nothing; see max_layers for deep nesting.
        void push_layer();
        void pop_layer(float opacity);

        // The pixels as rgba8, sRGB-encoded, straight alpha. Open layers are
        // ignored.
        image to_image() const;

    private:
        canvas() = default;

        std::uint32_t m_width = 0;
        std::uint32_t m_height = 0;
        bool m_linear = false;
        std::vector<std::vector<float>> m_layers;
        std::size_t m_ignored_layers = 0; // pushed beyond max_layers
        std::vector<float> m_accumulation;
    };

} // namespace volt::image
