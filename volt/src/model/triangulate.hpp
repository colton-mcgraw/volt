#pragma once

#include <math/vector.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// Private to volt_model: triangulation of polygons with holes in the plane,
// and a triangle mesh over them that can be refined.
//
// triangulate_polygon() is ear clipping with holes bridged to the outer ring,
// after mapbox's earcut (David Eberly's method, with a z-order index for
// large rings). Unlike earcut it never loses a vertex: one it skips because
// it lies on a straight stretch of the ring is put back into the edge it lies
// on afterwards. Every vertex of every ring ends up in the triangulation,
// which is what keeps two faces that share an edge watertight.
//
// mesh2d is that triangulation with neighbors: Lawson flips towards
// Delaunay, and insertion of new points, never touching the boundary edges.

namespace volt::model::geometry
{

    using triangle_indices = std::array<std::uint32_t, 3>;

    // rings[0] is the outer ring, the others holes, each a cycle of indices
    // into points; either orientation. The triangles come out
    // counter-clockwise. `clean` is cleared when part of the polygon could
    // only be filled by a fallback (it intersects itself).
    std::vector<triangle_indices> triangulate_polygon(const std::vector<math::vec2d> &points,
                                                      const std::vector<std::vector<std::uint32_t>> &rings,
                                                      bool &clean);

    // Twice the signed area of triangle abc: positive counter-clockwise.
    inline double orient(const math::vec2d &a, const math::vec2d &b, const math::vec2d &c)
    {
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    }

    // Counter-clockwise and not almost flat: twice the area against the
    // square of the longest side, which is the sine of the smallest angle
    // give or take. Anything flatter is a sliver made of rounding noise.
    inline bool proper(const math::vec2d &a, const math::vec2d &b, const math::vec2d &c)
    {
        const auto square = [](const math::vec2d &p, const math::vec2d &q) {
            return (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]);
        };
        const double longest = std::max({square(a, b), square(b, c), square(c, a)});
        return orient(a, b, c) > 1e-6 * longest;
    }

    class mesh2d
    {
    public:
        struct triangle
        {
            // Counter-clockwise. n[i] is the triangle across the edge from
            // v[i] to v[i + 1], or -1 on the boundary.
            std::array<std::uint32_t, 3> v{};
            std::array<std::int32_t, 3> n{-1, -1, -1};
        };

        std::vector<math::vec2d> points;
        std::vector<triangle> triangles;

        // Neighbors from shared edges. An edge used by two triangles in
        // opposite directions is interior; anything else is boundary.
        void connect();

        // Flips every interior edge that is not locally Delaunay, until none
        // is left (or a generous limit of flips is reached).
        void make_delaunay();

        // A new point inside triangle t; returns its index. The triangles
        // that changed are appended to `touched`.
        std::uint32_t insert(std::uint32_t t, const math::vec2d &p, std::vector<std::uint32_t> &touched);

        // A new point on the interior edge `edge` of triangle t (between
        // v[edge] and v[edge + 1]).
        std::uint32_t split(std::uint32_t t, int edge, const math::vec2d &p, std::vector<std::uint32_t> &touched);

        // Whether inserting p into triangle t, or splitting its edge at p,
        // leaves only proper triangles.
        bool can_insert(std::uint32_t t, const math::vec2d &p) const;
        bool can_split(std::uint32_t t, int edge, const math::vec2d &p) const;

    private:
        void legalize(std::vector<std::pair<std::uint32_t, int>> &stack, std::vector<std::uint32_t> *touched);
        void relink(std::int32_t triangle, std::int32_t from, std::int32_t to);
        bool flip(std::uint32_t t, int edge);
    };

} // namespace volt::model::geometry
