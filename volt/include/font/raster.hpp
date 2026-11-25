#pragma once

#include <font/font.hpp>
#include <font/outline.hpp>
#include <math/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// raster
// -------------------------------------------------------------------------
//
// Outlines to pixels, in two forms.
//
// Coverage: each pixel's value is the fraction of its square the shape
// covers, computed exactly for the polygon the outline flattens to (curves
// are flattened to within raster_tolerance first) by accumulating signed
// areas per cell and summing along each row. No hinting, no gamma: coverage
// is linear, 0 to 255 for 0 to 1, and meant to be used as alpha when
// blending in linear light (see <gpu/color.hpp>).
//
// Distance field: each pixel's value is its distance to the outline, signed
// (inside positive), for drawing text at any scale from one small image by
// thresholding at the edge in a shader. See distance_field() for what that
// can and cannot do.
//
// Images are rows top to bottom, y down, one byte per pixel, no padding
// between rows. They are placed relative to the pen position on the
// baseline: pixel (0, 0)'s top-left corner is at pen + offset.

namespace volt::font
{

    struct glyph_image
    {
        int width = 0;
        int height = 0;

        // Where the image's top-left corner goes relative to the pen
        // position on the baseline, in whole pixels, y down. Usually
        // negative in y: most of a glyph is above the baseline.
        math::vec2i offset{};

        std::vector<std::uint8_t> pixels;

        bool empty() const noexcept { return width == 0 || height == 0; }
        std::uint8_t at(int x, int y) const noexcept
        {
            return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
        }
    };

    // Images larger than this either way are refused: a size that big is a
    // mistake (or a hostile outline), not text.
    inline constexpr int max_image_size = 4096;

    // How far, in pixels, the polygon drawn may stray from the curves. The
    // polygon's chords cut inside the curves, so this is also roughly how
    // much thinner than exact the strokes come out: 1/64 keeps the area
    // within half a percent of exact even at 12 px.
    inline constexpr float raster_tolerance = 1.0f / 64.0f;

    // ---------------------------------------------------------------------
    // rasterizer
    // ---------------------------------------------------------------------
    //
    // The coverage accumulator itself, for drawing anything made of closed
    // outlines into an image of one's own: a whole line of text, a test
    // specimen. Keeps its buffer between uses.
    class rasterizer
    {
    public:
        // Starts over with an empty width x height canvas.
        void reset(int width, int height);

        int width() const noexcept { return m_width; }
        int height() const noexcept { return m_height; }

        // One edge, in pixels, y down. Edges must form closed loops for the
        // result to mean anything; parts outside the canvas are clamped to
        // its border, which keeps the area of what is inside right.
        void line(const math::vec2f &from, const math::vec2f &to) noexcept;

        // Every contour of `shape`, mapped into pixels by `transform`.
        // Curves are flattened to within `tolerance` pixels.
        void add(const outline &shape, const affine &transform, float tolerance = raster_tolerance);

        // Coverage under the nonzero rule, 0 to 255, into `out`, which
        // holds height rows of `stride` bytes. Composes with what is
        // already there by keeping the larger value, so several resolves
        // can share an image.
        void resolve(std::span<std::uint8_t> out, std::size_t stride) const noexcept;

        // The same as fractions, width * height of them, not clamped to 1
        // where contours overlap: the sum is the exact area drawn.
        void resolve(std::span<float> out) const noexcept;

    private:
        int m_width = 0;
        int m_height = 0;
        std::size_t m_stride = 0;
        std::vector<float> m_cells;
        polygon m_scratch;
    };

    // ---------------------------------------------------------------------
    // Glyph images
    // ---------------------------------------------------------------------

    // Coverage of `shape` (font units, y up) at `scale` pixels per font
    // unit, moved right by `shift` pixels first: the subpixel position of
    // the pen, in [0, 1). The image is as small as the shape allows. Empty
    // for an empty shape.
    std::expected<glyph_image, std::string> rasterize(const outline &shape, float scale, float shift = 0.0f);

    // The same for a glyph of a font at `pixel_size` pixels per em.
    std::expected<glyph_image, std::string> rasterize(const font &font, glyph_id glyph, float pixel_size,
                                                      float shift = 0.0f);

    // A single-channel signed distance field of `shape` at `scale` pixels
    // per font unit, padded by `spread` pixels on every side. Each value is
    //     clamp(0.5 + distance / (2 * spread), 0, 1) * 255
    // with distance in pixels of this image, positive inside: 128 is the
    // edge, 0 and 255 are `spread` or more outside and inside. To draw it at
    // another size, sample bilinearly and threshold at 0.5 with a smoothstep
    // a pixel of the target wide.
    //
    // The distance is computed exactly to the outline flattened within 1/64
    // pixel, and the sign from the nonzero winding at each pixel center, so
    // the field is right to well under a pixel. What a single channel cannot
    // keep is a sharp corner: magnified more than about four times, corners
    // come out rounded, by roughly the size of one source pixel. Generate at
    // a larger size (48 to 64 px per em) when zooming far in matters.
    std::expected<glyph_image, std::string> distance_field(const outline &shape, float scale, float spread);
    std::expected<glyph_image, std::string> distance_field(const font &font, glyph_id glyph, float pixel_size,
                                                           float spread);

} // namespace volt::font
