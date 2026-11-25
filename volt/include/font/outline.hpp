#pragma once

#include <math/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// -------------------------------------------------------------------------
// outline
// -------------------------------------------------------------------------
//
// A glyph as geometry: closed contours of straight lines and quadratic or
// cubic Bezier curves. TrueType glyphs are made of lines and quadratics, CFF
// glyphs of lines and cubics; nothing here cares which.
//
// The coordinate system is whatever the outline was built in. Outlines that
// come out of a font are in font units with y up and the origin at the pen
// position on the baseline (see font::units_per_em). transformed() moves them
// anywhere else, for example into pixels with y down.
//
// Filled with the nonzero winding rule. Contours may overlap (variable fonts
// and many composites do), and their direction is not normalized: TrueType
// draws outer contours clockwise in y up, CFF counterclockwise.

namespace volt::font
{

    enum class segment_kind : std::uint8_t
    {
        line,
        quadratic,
        cubic,
    };

    // One piece of a contour. Each segment carries its own start, so a
    // segment can be used without the one before it; `from` is always the
    // previous segment's `to`.
    struct segment
    {
        segment_kind kind = segment_kind::line;
        math::vec2f from{};
        math::vec2f control_a{}; // quadratic: its one control point; cubic: the first
        math::vec2f control_b{}; // cubic: the second; unused otherwise
        math::vec2f to{};

        // The point at parameter t in [0, 1].
        math::vec2f at(float t) const noexcept;
    };

    // An axis-aligned rectangle. Empty (min > max) until something is added.
    struct box
    {
        math::vec2f min{1.0f, 1.0f};
        math::vec2f max{-1.0f, -1.0f};

        bool empty() const noexcept { return min[0] > max[0] || min[1] > max[1]; }
        float width() const noexcept { return empty() ? 0.0f : max[0] - min[0]; }
        float height() const noexcept { return empty() ? 0.0f : max[1] - min[1]; }

        void add(const math::vec2f &point) noexcept;
        void merge(const box &other) noexcept;

        friend bool operator==(const box &, const box &) = default;
    };

    // Closed polygons, the outline after flattening: contour i is
    // points[ends[i - 1]] to points[ends[i] - 1] (from 0 for the first), and
    // its last point connects back to its first. The closing point is not
    // repeated.
    struct polygon
    {
        std::vector<math::vec2f> points;
        std::vector<std::uint32_t> ends;

        std::size_t contour_count() const noexcept { return ends.size(); }
        std::span<const math::vec2f> contour(std::size_t index) const noexcept;
    };

    // A 2D affine map: p' = (xx * x + xy * y, yx * x + yy * y) + offset.
    struct affine
    {
        float xx = 1.0f;
        float xy = 0.0f;
        float yx = 0.0f;
        float yy = 1.0f;
        math::vec2f offset{};

        math::vec2f apply(const math::vec2f &p) const noexcept
        {
            return {xx * p[0] + xy * p[1] + offset[0], yx * p[0] + yy * p[1] + offset[1]};
        }

        // Scale, then translate. A negative y scale turns font units (y up)
        // into pixels (y down).
        static affine scale(float x, float y, const math::vec2f &offset = {}) noexcept
        {
            return {x, 0.0f, 0.0f, y, offset};
        }
    };

    // ---------------------------------------------------------------------
    // outline
    // ---------------------------------------------------------------------
    class outline
    {
    public:
        // ---- building ----------------------------------------------------
        //
        // Like a path API: move_to() starts a contour (and closes the one
        // before), the others continue it from the current point. A contour
        // is closed with a line back to its start if it does not end there.
        // Drawing commands before the first move_to() start at the origin.

        void move_to(const math::vec2f &point);
        void line_to(const math::vec2f &point);
        void quadratic_to(const math::vec2f &control, const math::vec2f &point);
        void cubic_to(const math::vec2f &control_a, const math::vec2f &control_b, const math::vec2f &point);
        void close();

        // Appends `other`'s contours, mapped through `transform`.
        void append(const outline &other, const affine &transform = {});

        void clear();

        // ---- reading -----------------------------------------------------

        bool empty() const noexcept { return m_segments.empty(); }
        std::size_t contour_count() const noexcept { return m_ends.size(); }
        std::span<const segment> contour(std::size_t index) const noexcept;
        std::span<const segment> segments() const noexcept { return m_segments; }

        // The box around every point including control points. For a glyph
        // from a 'glyf' table, this is exactly the box in the glyph's header.
        box control_bounds() const noexcept;

        // The tight box around the curves themselves, which can be smaller
        // than control_bounds() where a control point lies outside.
        box bounds() const noexcept;

        // The signed area enclosed, exact for the curves (not a polygon
        // approximation). Positive for counterclockwise contours in a y up
        // system. Overlapping regions count once per contour covering them.
        float signed_area() const noexcept;

        outline transformed(const affine &transform) const;

        // Lines within `tolerance` (in this outline's units) of the curves.
        // Curves are split evenly into as many pieces as the tolerance needs,
        // at most 1024 per curve, so a degenerate outline cannot run away.
        polygon flatten(float tolerance) const;
        void flatten(float tolerance, polygon &out) const;

    private:
        void finish_contour();

        std::vector<segment> m_segments;
        std::vector<std::uint32_t> m_ends;

        // The contour being built: where it started, and where it is now.
        math::vec2f m_start{};
        math::vec2f m_current{};
        bool m_open = false;
    };

    // How many straight pieces a curve needs so that none strays more than
    // `tolerance` from it (Wang's formula). At least 1, at most 1024.
    std::uint32_t subdivisions(const segment &segment, float tolerance) noexcept;

} // namespace volt::font
