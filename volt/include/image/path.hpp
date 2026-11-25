#pragma once

#include <math/matrix.hpp>
#include <math/vector.hpp>

#include <cstdint>
#include <span>
#include <vector>

// -------------------------------------------------------------------------
// path
// -------------------------------------------------------------------------
//
// Vector outlines as SVG, PostScript and fonts describe them: subpaths of
// lines and quadratic and cubic Bézier curves, each open or closed. The
// geometry is independent of any device; flatten() turns it into polygons
// at a tolerance, and stroke() turns a path into the outline its stroke
// covers, so that both fills and strokes come down to filling polygons
// (see <image/raster.hpp>).
//
// Coordinates are whatever the caller's are: SVG user units, schematic
// millimeters. Transforms are 3x3 homogeneous affine matrices
// (math::mat3f), applied as m * p, as everywhere in volt::math.

namespace volt::image
{

    enum class path_verb : std::uint8_t
    {
        move,  // 1 point: starts a subpath
        line,  // 1 point
        quad,  // 2 points: control, end
        cubic, // 3 points: control, control, end
        close, // no points: back to the subpath's start
    };

    class path
    {
    public:
        void move_to(math::vec2f point);
        void line_to(math::vec2f point);
        void quad_to(math::vec2f control, math::vec2f point);
        void cubic_to(math::vec2f control1, math::vec2f control2, math::vec2f point);
        void close();

        // An elliptical arc to `point`, in SVG's endpoint form: radii, the
        // x axis rotation in degrees, and the two flags choosing among the
        // four arcs. Appended as cubic curves (one per quarter turn or
        // less, within 0.03% of the ellipse). Out-of-range radii are
        // scaled up and zero radii make a line, as SVG specifies.
        void arc_to(math::vec2f radii, float rotation_degrees, bool large_arc, bool sweep, math::vec2f point);

        // Whole closed shapes, each its own subpath. Ellipses and rounded
        // corners are cubic curves; radii are clamped to half the sides.
        void add_rectangle(math::vec2f corner, math::vec2f size);
        void add_rounded_rectangle(math::vec2f corner, math::vec2f size, math::vec2f radii);
        void add_ellipse(math::vec2f center, math::vec2f radii);

        // Everything in `other`, transformed.
        void append(const path &other, const math::mat3f &transform);

        bool empty() const noexcept { return m_verbs.empty(); }
        void clear() noexcept;

        std::span<const path_verb> verbs() const noexcept { return m_verbs; }
        std::span<const math::vec2f> points() const noexcept { return m_points; }

        // Where the next segment starts; (0, 0) for an empty path.
        math::vec2f current_point() const noexcept { return m_current; }

        // The exact bounds of the geometry (curve extrema, not control
        // points), as {min, max}. Both zero for an empty path.
        struct bounding_box
        {
            math::vec2f min;
            math::vec2f max;
        };
        bounding_box bounds() const;

    private:
        // A segment needs a subpath to be in: after close() (or at the very
        // start) one is begun at the current point.
        void ensure_subpath();

        std::vector<path_verb> m_verbs;
        std::vector<math::vec2f> m_points;
        math::vec2f m_current{};
        math::vec2f m_start{};
        bool m_open = false; // a subpath is in progress
    };

    // ---- polygons ---------------------------------------------------------

    // A flattened subpath: a polyline, or a polygon when closed. A subpath
    // that was drawn but has no length (SVG's "M 10 10 Z", which still gets
    // round or square caps) keeps one point.
    struct contour
    {
        std::vector<math::vec2f> points;
        bool closed = false;
    };

    // The path transformed and flattened so that no point of the polylines
    // is further than `tolerance` (in transformed units) from the curves.
    // A lone move_to makes no contour.
    std::vector<contour> flatten(const path &source, const math::mat3f &transform, float tolerance);

    // ---- strokes ----------------------------------------------------------

    enum class fill_rule : std::uint8_t
    {
        nonzero,
        even_odd,
    };

    enum class line_cap : std::uint8_t
    {
        butt,
        round,
        square,
    };

    enum class line_join : std::uint8_t
    {
        miter,
        round,
        bevel,
    };

    struct stroke_style
    {
        float width = 1.0f;
        line_cap cap = line_cap::butt;
        line_join join = line_join::miter;

        // Miters longer than this many times the width become bevels.
        float miter_limit = 4.0f;

        // Alternating dash and gap lengths; empty for a solid line. A
        // pattern of odd length repeats twice, as SVG says; one with
        // negative entries or a zero sum draws a solid line.
        std::vector<float> dashes;
        float dash_offset = 0.0f;
    };

    // The area the stroke covers, as polygons to fill with the nonzero
    // rule, in the path's own coordinates: a polygon per segment, join and
    // cap, all wound the same way so that where they overlap they add up
    // rather than cancel. `tolerance` bounds the error of curves and round
    // parts, in path units; pass the device tolerance divided by the
    // transform's scale. Transform the result for the device; that is also
    // what makes strokes under non-uniform scaling come out as SVG wants.
    std::vector<contour> stroke(const path &source, const stroke_style &style, float tolerance);

    // Polylines cut into dashes: `pattern` alternates on and off lengths,
    // starting `offset` into it. Every dash comes back as an open contour.
    std::vector<contour> dash(std::span<const contour> lines, std::span<const float> pattern, float offset);

    // ---- transforms -------------------------------------------------------

    // The largest factor by which the matrix stretches any direction: what
    // turns a tolerance in device units into one in user units.
    float max_scale(const math::mat3f &transform) noexcept;

    math::vec2f apply(const math::mat3f &transform, math::vec2f point) noexcept;

} // namespace volt::image
