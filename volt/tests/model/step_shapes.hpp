#pragma once

#include "step_author.hpp"

#include <cmath>
#include <numbers>
#include <string>

// The STEP files the tests read, one function each, with the analytic volume
// each should come out with. Topology as OpenCASCADE writes it: full circles
// as closed edges on one vertex, seam edges where a periodic surface closes,
// edges shared by the two faces they bound and used in opposite directions.

namespace volt::tests::shapes
{
    using vec3 = math::vec3d;
    inline constexpr double pi = std::numbers::pi;

    // Eight corners, twelve straight edges, four side faces; the loops for
    // bottom and top so that a caller can add holes or use another surface.
    struct box_parts
    {
        int vertices[8] = {};
        int edges[13] = {}; // 1-based, e1..e12 as in assets/models/tests/cube.step
        int bottom_loop = 0;
        int top_loop = 0;
        std::vector<int> sides;
    };

    inline box_parts box(step_author &a, const vec3 &lo, const vec3 &hi, bool make_top_edges = true)
    {
        box_parts b;
        const vec3 corner[8] = {{lo[0], lo[1], lo[2]}, {hi[0], lo[1], lo[2]}, {hi[0], hi[1], lo[2]},
                                {lo[0], hi[1], lo[2]}, {lo[0], lo[1], hi[2]}, {hi[0], lo[1], hi[2]},
                                {hi[0], hi[1], hi[2]}, {lo[0], hi[1], hi[2]}};
        for (int i = 0; i < 8; ++i)
            b.vertices[i] = a.vertex(corner[i]);
        const int ends[13][2] = {{0, 0}, {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                 {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (int e = 1; e <= 12; ++e)
        {
            if (!make_top_edges && e >= 5 && e <= 8)
                continue;
            b.edges[e] = a.edge(b.vertices[ends[e][0]], b.vertices[ends[e][1]],
                                a.line(corner[ends[e][0]], corner[ends[e][1]]));
        }
        b.bottom_loop = a.loop({{b.edges[4], false}, {b.edges[3], false}, {b.edges[2], false}, {b.edges[1], false}});
        if (make_top_edges)
            b.top_loop = a.loop({{b.edges[5], true}, {b.edges[6], true}, {b.edges[7], true}, {b.edges[8], true}});
        return b;
    }

    inline void box_sides(step_author &a, box_parts &b, const vec3 &lo, const vec3 &hi)
    {
        const auto e = [&](int i) { return b.edges[i]; };
        b.sides.push_back(a.face({a.bound(a.loop({{e(1), true}, {e(10), true}, {e(5), false}, {e(9), false}}))},
                                 a.plane(lo, {0, -1, 0})));
        b.sides.push_back(a.face({a.bound(a.loop({{e(2), true}, {e(11), true}, {e(6), false}, {e(10), false}}))},
                                 a.plane({hi[0], lo[1], lo[2]}, {1, 0, 0}, {0, 1, 0})));
        b.sides.push_back(a.face({a.bound(a.loop({{e(3), true}, {e(12), true}, {e(7), false}, {e(11), false}}))},
                                 a.plane({lo[0], hi[1], lo[2]}, {0, 1, 0})));
        b.sides.push_back(a.face({a.bound(a.loop({{e(4), true}, {e(9), true}, {e(8), false}, {e(12), false}}))},
                                 a.plane(lo, {-1, 0, 0}, {0, 1, 0})));
    }

    inline int box_solid(step_author &a, const vec3 &lo, const vec3 &hi)
    {
        box_parts b = box(a, lo, hi);
        box_sides(a, b, lo, hi);
        std::vector<int> faces = b.sides;
        faces.push_back(a.face({a.bound(b.bottom_loop)}, a.plane(lo, {0, 0, 1}), false));
        faces.push_back(a.face({a.bound(b.top_loop)}, a.plane(hi, {0, 0, 1})));
        return a.solid(faces);
    }

    // ---- cylinder: two planar caps, a lateral face closed by a seam -------

    inline int cylinder_solid(step_author &a, const vec3 &base, double r, double h)
    {
        const vec3 z{0, 0, 1};
        const vec3 x{1, 0, 0};
        const vec3 top{base[0], base[1], base[2] + h};
        const int surface = a.add(std::format("CYLINDRICAL_SURFACE('',{},{})", step_author::ref(a.placement(base, z, x)),
                                              step_author::real(r)));
        const int vb = a.vertex(base + x * r);
        const int vt = a.vertex(top + x * r);
        const int eb = a.surface_edge(vb, vb, a.circle(base, z, x, r), surface);
        const int et = a.surface_edge(vt, vt, a.circle(top, z, x, r), surface);
        const int seam = a.surface_edge(vb, vt, a.line(base + x * r, top + x * r), surface, true);
        const int lateral = a.face({a.bound(a.loop({{eb, true}, {seam, true}, {et, false}, {seam, false}}))}, surface);
        const int cap_top = a.face({a.bound(a.loop({{et, true}}))}, a.plane(top, z));
        const int cap_bottom = a.face({a.bound(a.loop({{eb, false}}))}, a.plane(base, z), false);
        return a.solid({lateral, cap_top, cap_bottom});
    }

    inline std::string cylinder(double r = 1.0, double h = 2.0)
    {
        step_author a;
        const int solid = cylinder_solid(a, {0, 0, 0}, r, h);
        a.part("Cylinder", {solid});
        a.color(solid, {0.7, 0.7, 0.75});
        return a.text("cylinder.step");
    }

    // ---- a plate with a cylindrical through hole --------------------------
    //
    // The hole's face is bounded by its two circles alone, no seam: a loop at
    // each end, running once around the cylinder.

    inline std::string plate_with_hole(double width = 10.0, double depth = 8.0, double thickness = 2.0,
                                       double hole = 1.5)
    {
        step_author a;
        const vec3 lo{0, 0, 0};
        const vec3 hi{width, depth, thickness};
        const vec3 centre{width / 2, depth / 2, 0};
        const vec3 z{0, 0, 1};
        const vec3 x{1, 0, 0};

        box_parts b = box(a, lo, hi);
        box_sides(a, b, lo, hi);

        const vec3 top_centre{centre[0], centre[1], thickness};
        const int hb = a.vertex(centre + x * hole);
        const int ht = a.vertex(top_centre + x * hole);
        const int eb = a.edge(hb, hb, a.circle(centre, z, x, hole));
        const int et = a.edge(ht, ht, a.circle(top_centre, z, x, hole));

        const int bore = a.add(std::format("CYLINDRICAL_SURFACE('',{},{})",
                                           step_author::ref(a.placement(centre, z, x)), step_author::real(hole)));
        std::vector<int> faces = b.sides;
        faces.push_back(a.face({a.bound(b.top_loop), a.bound(a.loop({{et, false}}), false)}, a.plane(hi, z)));
        faces.push_back(a.face({a.bound(b.bottom_loop), a.bound(a.loop({{eb, true}}), false)}, a.plane(lo, z), false));
        faces.push_back(a.face({a.bound(a.loop({{et, true}}), false), a.bound(a.loop({{eb, false}}), false)}, bore,
                               false));
        const int solid = a.solid(faces);
        a.part("Plate", {solid});
        a.color(solid, {0.1, 0.5, 0.2});
        return a.text("plate_with_hole.step");
    }

    // ---- cones ------------------------------------------------------------
    //
    // A frustum, radius 2 at the bottom and 1 at the top, 2 high: the cone's
    // axis points down from the top, so that its radius grows along it.

    inline std::string cone_frustum(double r_bottom = 2.0, double r_top = 1.0, double h = 2.0)
    {
        step_author a;
        const vec3 down{0, 0, -1};
        const vec3 x{1, 0, 0};
        const vec3 top{0, 0, h};
        const vec3 bottom{0, 0, 0};
        const double semi_angle = std::atan((r_bottom - r_top) / h);
        const int surface = a.add(std::format("CONICAL_SURFACE('',{},{},{})", step_author::ref(a.placement(top, down, x)),
                                              step_author::real(r_top), step_author::real(semi_angle)));
        const int vt = a.vertex(top + x * r_top);
        const int vb = a.vertex(bottom + x * r_bottom);
        const int et = a.surface_edge(vt, vt, a.circle(top, down, x, r_top), surface);
        const int eb = a.surface_edge(vb, vb, a.circle(bottom, down, x, r_bottom), surface);
        const int seam = a.surface_edge(vt, vb, a.line(top + x * r_top, bottom + x * r_bottom), surface, true);
        const int lateral = a.face({a.bound(a.loop({{et, true}, {seam, true}, {eb, false}, {seam, false}}))}, surface);
        const int cap_top = a.face({a.bound(a.loop({{et, false}}))}, a.plane(top, {0, 0, 1}));
        const int cap_bottom = a.face({a.bound(a.loop({{eb, true}}))}, a.plane(bottom, {0, 0, 1}), false);
        const int solid = a.solid({lateral, cap_top, cap_bottom});
        a.part("Frustum", {solid});
        return a.text("cone_frustum.step");
    }

    // A cone that comes to a point: the apex is a vertex the seam runs to.
    inline std::string cone_point(double r = 1.0, double h = 2.0)
    {
        step_author a;
        const vec3 down{0, 0, -1};
        const vec3 x{1, 0, 0};
        const vec3 apex{0, 0, h};
        const vec3 bottom{0, 0, 0};
        const int surface = a.add(std::format("CONICAL_SURFACE('',{},0.,{})", step_author::ref(a.placement(apex, down, x)),
                                              step_author::real(std::atan(r / h))));
        const int va = a.vertex(apex);
        const int vb = a.vertex(bottom + x * r);
        const int eb = a.surface_edge(vb, vb, a.circle(bottom, down, x, r), surface);
        const int seam = a.surface_edge(vb, va, a.line(bottom + x * r, apex), surface, true);
        const int lateral = a.face({a.bound(a.loop({{seam, false}, {eb, false}, {seam, true}}))}, surface);
        const int cap = a.face({a.bound(a.loop({{eb, true}}))}, a.plane(bottom, {0, 0, 1}), false);
        const int solid = a.solid({lateral, cap});
        a.part("Cone", {solid});
        return a.text("cone_point.step");
    }

    // ---- sphere: one face, a seam from pole to pole ------------------------

    inline std::string sphere(double r = 1.5)
    {
        step_author a;
        const vec3 origin{0, 0, 0};
        const int surface = a.add(std::format("SPHERICAL_SURFACE('',{},{})", step_author::ref(a.placement(origin)),
                                              step_author::real(r)));
        const int south = a.vertex({0, 0, -r});
        const int north = a.vertex({0, 0, r});
        const int meridian = a.circle(origin, {0, -1, 0}, {1, 0, 0}, r);
        const int seam = a.surface_edge(south, north, meridian, surface, true);
        const int face = a.face({a.bound(a.loop({{seam, true}, {seam, false}}))}, surface);
        const int solid = a.solid({face});
        a.part("Sphere", {solid});
        a.color(solid, {0.9, 0.6, 0.1});
        return a.text("sphere.step");
    }

    // A pin with a round head: a cylinder capped by a hemisphere whose face
    // is bounded by the circle alone, the pole inside it.
    inline std::string capsule(double r = 1.0, double h = 2.0)
    {
        step_author a;
        const vec3 z{0, 0, 1};
        const vec3 x{1, 0, 0};
        const vec3 base{0, 0, 0};
        const vec3 top{0, 0, h};
        const int cylinder = a.add(std::format("CYLINDRICAL_SURFACE('',{},{})",
                                               step_author::ref(a.placement(base, z, x)), step_author::real(r)));
        const int dome = a.add(std::format("SPHERICAL_SURFACE('',{},{})", step_author::ref(a.placement(top, z, x)),
                                           step_author::real(r)));
        const int vb = a.vertex(base + x * r);
        const int vt = a.vertex(top + x * r);
        const int eb = a.edge(vb, vb, a.circle(base, z, x, r));
        const int et = a.edge(vt, vt, a.circle(top, z, x, r));
        const int seam = a.edge(vb, vt, a.line(base + x * r, top + x * r));
        const int lateral = a.face({a.bound(a.loop({{eb, true}, {seam, true}, {et, false}, {seam, false}}))}, cylinder);
        const int head = a.face({a.bound(a.loop({{et, true}}))}, dome);
        const int cap = a.face({a.bound(a.loop({{eb, false}}))}, a.plane(base, z), false);
        const int solid = a.solid({lateral, head, cap});
        a.part("Capsule", {solid});
        return a.text("capsule.step");
    }

    // ---- torus: one face, closed by two seams -----------------------------

    inline std::string torus(double major = 3.0, double minor = 1.0)
    {
        step_author a;
        const vec3 origin{0, 0, 0};
        const int surface =
            a.add(std::format("TOROIDAL_SURFACE('',{},{},{})", step_author::ref(a.placement(origin)),
                              step_author::real(major), step_author::real(minor)));
        const int v = a.vertex({major + minor, 0, 0});
        const int around = a.surface_edge(v, v, a.circle(origin, {0, 0, 1}, {1, 0, 0}, major + minor), surface, true);
        const int tube = a.surface_edge(v, v, a.circle({major, 0, 0}, {0, -1, 0}, {1, 0, 0}, minor), surface, true);
        const int face = a.face({a.bound(a.loop({{around, true}, {tube, true}, {around, false}, {tube, false}}))}, surface);
        const int solid = a.solid({face});
        a.part("Torus", {solid});
        return a.text("torus.step");
    }

    // ---- swept surfaces ---------------------------------------------------

    // The cone frustum again, its side a line turned about the axis.
    inline std::string revolved_frustum(double r_bottom = 2.0, double r_top = 1.0, double h = 2.0)
    {
        step_author a;
        const vec3 z{0, 0, 1};
        const vec3 x{1, 0, 0};
        const vec3 bottom{0, 0, 0};
        const vec3 top{0, 0, h};
        const int profile = a.line(bottom + x * r_bottom, top + x * r_top);
        const int axis = a.add(std::format("AXIS1_PLACEMENT('',{},{})", step_author::ref(a.point(bottom)),
                                           step_author::ref(a.direction(z))));
        const int surface = a.add(std::format("SURFACE_OF_REVOLUTION('',{},{})", step_author::ref(profile),
                                              step_author::ref(axis)));
        const int vb = a.vertex(bottom + x * r_bottom);
        const int vt = a.vertex(top + x * r_top);
        const int eb = a.edge(vb, vb, a.circle(bottom, z, x, r_bottom));
        const int et = a.edge(vt, vt, a.circle(top, z, x, r_top));
        const int seam = a.edge(vb, vt, profile);
        const int lateral = a.face({a.bound(a.loop({{eb, true}, {seam, true}, {et, false}, {seam, false}}))}, surface);
        const int cap_top = a.face({a.bound(a.loop({{et, true}}))}, a.plane(top, z));
        const int cap_bottom = a.face({a.bound(a.loop({{eb, false}}))}, a.plane(bottom, z), false);
        const int solid = a.solid({lateral, cap_top, cap_bottom});
        a.part("Revolved", {solid});
        return a.text("revolved_frustum.step");
    }

    // An elliptic cylinder: an ellipse swept along z.
    inline std::string extruded_ellipse(double semi_a = 2.0, double semi_b = 1.0, double h = 3.0)
    {
        step_author a;
        const vec3 z{0, 0, 1};
        const vec3 x{1, 0, 0};
        const vec3 bottom{0, 0, 0};
        const vec3 top{0, 0, h};
        const auto ellipse = [&](const vec3 &centre) {
            return a.add(std::format("ELLIPSE('',{},{},{})", step_author::ref(a.placement(centre, z, x)),
                                     step_author::real(semi_a), step_author::real(semi_b)));
        };
        const int profile = ellipse(bottom);
        const int sweep = a.add(std::format("VECTOR('',{},1.)", step_author::ref(a.direction(z))));
        const int surface = a.add(std::format("SURFACE_OF_LINEAR_EXTRUSION('',{},{})", step_author::ref(profile),
                                              step_author::ref(sweep)));
        const int vb = a.vertex(bottom + x * semi_a);
        const int vt = a.vertex(top + x * semi_a);
        const int eb = a.edge(vb, vb, profile);
        const int et = a.edge(vt, vt, ellipse(top));
        const int seam = a.edge(vb, vt, a.line(bottom + x * semi_a, top + x * semi_a));
        const int lateral = a.face({a.bound(a.loop({{eb, true}, {seam, true}, {et, false}, {seam, false}}))}, surface);
        const int cap_top = a.face({a.bound(a.loop({{et, true}}))}, a.plane(top, z));
        const int cap_bottom = a.face({a.bound(a.loop({{eb, false}}))}, a.plane(bottom, z), false);
        const int solid = a.solid({lateral, cap_top, cap_bottom});
        a.part("Extruded", {solid});
        return a.text("extruded_ellipse.step");
    }

    // ---- B-splines --------------------------------------------------------
    //
    // A 4 x 4 block whose top is a bicubic Bezier patch: control points
    // evenly spaced in x and y, z = 1 around the rim and `bump` at the four
    // inner ones. With even spacing x and y are linear in the parameters, so
    // the volume under the patch is the area times the mean control height.

    inline double bspline_block_volume(double bump = 2.0)
    {
        return 16.0 * (12.0 * 1.0 + 4.0 * bump) / 16.0;
    }

    inline std::string bspline_block(double bump = 2.0)
    {
        step_author a;
        const vec3 lo{0, 0, 0};
        const vec3 hi{4, 4, 1};
        box_parts b = box(a, lo, hi, false);

        int grid[4][4];
        for (int i = 0; i < 4; ++i)
        {
            for (int j = 0; j < 4; ++j)
            {
                const bool inner = i > 0 && i < 3 && j > 0 && j < 3;
                grid[i][j] = a.point({4.0 * i / 3.0, 4.0 * j / 3.0, inner ? bump : 1.0});
            }
        }
        std::string rows;
        for (int i = 0; i < 4; ++i)
        {
            rows += i == 0 ? "(" : ",(";
            for (int j = 0; j < 4; ++j)
                rows += (j == 0 ? "" : ",") + step_author::ref(grid[i][j]);
            rows += ")";
        }
        const int patch = a.add("B_SPLINE_SURFACE_WITH_KNOTS('',3,3,(" + rows +
                                "),.UNSPECIFIED.,.F.,.F.,.F.,(4,4),(4,4),(0.,1.),(0.,1.),.UNSPECIFIED.)");

        const auto curve = [&](int p0, int p1, int p2, int p3) {
            return a.add(std::format("B_SPLINE_CURVE_WITH_KNOTS('',3,({},{},{},{}),.UNSPECIFIED.,.F.,.F.,(4,4),"
                                     "(0.,1.),.UNSPECIFIED.)",
                                     step_author::ref(p0), step_author::ref(p1), step_author::ref(p2),
                                     step_author::ref(p3)));
        };
        b.edges[5] = a.edge(b.vertices[4], b.vertices[5], curve(grid[0][0], grid[1][0], grid[2][0], grid[3][0]));
        b.edges[6] = a.edge(b.vertices[5], b.vertices[6], curve(grid[3][0], grid[3][1], grid[3][2], grid[3][3]));
        b.edges[7] =
            a.edge(b.vertices[6], b.vertices[7], curve(grid[0][3], grid[1][3], grid[2][3], grid[3][3]), false);
        b.edges[8] =
            a.edge(b.vertices[7], b.vertices[4], curve(grid[0][0], grid[0][1], grid[0][2], grid[0][3]), false);
        box_sides(a, b, lo, hi);

        std::vector<int> faces = b.sides;
        faces.push_back(a.face({a.bound(b.bottom_loop)}, a.plane(lo, {0, 0, 1}), false));
        faces.push_back(a.face(
            {a.bound(a.loop({{b.edges[5], true}, {b.edges[6], true}, {b.edges[7], true}, {b.edges[8], true}}))},
            patch));
        const int solid = a.solid(faces);
        a.part("BsplineBlock", {solid});
        a.color(solid, {0.3, 0.4, 0.9});
        return a.text("bspline_block.step");
    }

    // A cylinder of radius 1, 2 high, whose side is a rational B-spline
    // surface and whose circles are rational B-spline curves: the complex
    // instances exporters write for NURBS.
    inline std::string rational_cylinder()
    {
        step_author a;
        const double s = std::sqrt(0.5);
        const double xy[9][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}};
        const double w[9] = {1, s, 1, s, 1, s, 1, s, 1};
        std::string weights;
        for (int i = 0; i < 9; ++i)
            weights += (i == 0 ? "" : ",") + step_author::real(w[i]);

        const auto circle = [&](double z) {
            std::string points;
            for (int i = 0; i < 9; ++i)
                points += (i == 0 ? "" : ",") + step_author::ref(a.point({xy[i][0], xy[i][1], z}));
            return a.add("( BOUNDED_CURVE() B_SPLINE_CURVE(2,(" + points +
                         "),.CIRCULAR_ARC.,.T.,.F.) B_SPLINE_CURVE_WITH_KNOTS((3,2,2,2,3),(0.,0.25,0.5,0.75,1.),"
                         ".UNSPECIFIED.) CURVE() GEOMETRIC_REPRESENTATION_ITEM() RATIONAL_B_SPLINE_CURVE((" +
                         weights + ")) REPRESENTATION_ITEM('') )");
        };

        std::string rows;
        std::string weight_rows;
        for (int i = 0; i < 9; ++i)
        {
            rows += std::format("{}({},{})", i == 0 ? "" : ",", step_author::ref(a.point({xy[i][0], xy[i][1], 0})),
                                step_author::ref(a.point({xy[i][0], xy[i][1], 2})));
            weight_rows += std::format("{}({},{})", i == 0 ? "" : ",", step_author::real(w[i]), step_author::real(w[i]));
        }
        const int surface = a.add("( BOUNDED_SURFACE() B_SPLINE_SURFACE(2,1,(" + rows +
                                  "),.UNSPECIFIED.,.T.,.F.,.F.) B_SPLINE_SURFACE_WITH_KNOTS((3,2,2,2,3),(2,2),"
                                  "(0.,0.25,0.5,0.75,1.),(0.,2.),.UNSPECIFIED.) GEOMETRIC_REPRESENTATION_ITEM() "
                                  "RATIONAL_B_SPLINE_SURFACE((" +
                                  weight_rows + ")) REPRESENTATION_ITEM('') SURFACE() )");

        const vec3 z{0, 0, 1};
        const int vb = a.vertex({1, 0, 0});
        const int vt = a.vertex({1, 0, 2});
        const int eb = a.edge(vb, vb, circle(0));
        const int et = a.edge(vt, vt, circle(2));
        const int seam = a.edge(vb, vt, a.line({1, 0, 0}, {1, 0, 2}));
        const int lateral = a.face({a.bound(a.loop({{eb, true}, {seam, true}, {et, false}, {seam, false}}))}, surface);
        const int cap_top = a.face({a.bound(a.loop({{et, true}}))}, a.plane({0, 0, 2}, z));
        const int cap_bottom = a.face({a.bound(a.loop({{eb, false}}))}, a.plane({0, 0, 0}, z), false);
        const int solid = a.solid({lateral, cap_top, cap_bottom});
        a.part("RationalCylinder", {solid});
        return a.text("rational_cylinder.step");
    }

    // ---- an assembly ------------------------------------------------------
    //
    // Two placements of one block (red), the second turned a quarter about z,
    // and a pin (blue).

    inline std::string assembly()
    {
        step_author a;
        const int block_solid = box_solid(a, {0, 0, 0}, {2, 1, 1});
        a.color(block_solid, {0.8, 0.1, 0.1});
        const auto block = a.part("Block", {block_solid});

        const int pin_solid = cylinder_solid(a, {0, 0, 0}, 0.5, 3.0);
        a.color(pin_solid, {0.1, 0.1, 0.8});
        const auto pin = a.part("Pin", {pin_solid});

        const int first = a.placement({0, 0, 0});
        const int second = a.placement({5, 0, 0}, {0, 0, 1}, {0, 1, 0});
        const int third = a.placement({2, 2, 0});
        const auto board = a.part("Board", {first, second, third}, "SHAPE_REPRESENTATION");
        a.occurrence(board, block, first, "B1");
        a.occurrence(board, block, second, "B2");
        a.occurrence(board, pin, third, "P1");
        return a.text("assembly.step");
    }

    // ---- AP242 tessellated geometry ---------------------------------------
    //
    // A unit cube as a TESSELLATED_SOLID: triangles listed directly, through
    // a point index, as a strip and as a fan.

    inline std::string tessellated_cube()
    {
        step_author a;
        const int coordinates = a.add("COORDINATES_LIST('',8,((0.,0.,0.),(1.,0.,0.),(1.,1.,0.),(0.,1.,0.),"
                                      "(0.,0.,1.),(1.,0.,1.),(1.,1.,1.),(0.,1.,1.)))");
        const std::string c = step_author::ref(coordinates);
        const int bottom = a.add("TRIANGULATED_FACE(''," + c + ",4,((0.,0.,-1.)),$,(),((1,4,3),(1,3,2)))");
        const int top = a.add("TRIANGULATED_FACE(''," + c + ",4,(),$,(5,6,7,8),((1,2,3),(1,3,4)))");
        const int front = a.add("COMPLEX_TRIANGULATED_FACE(''," + c + ",4,(),$,(),((1,2,5,6)),())");
        const int right = a.add("COMPLEX_TRIANGULATED_FACE(''," + c + ",4,(),$,(),(),((2,3,7,6)))");
        const int back = a.add("TRIANGULATED_FACE(''," + c + ",4,(),$,(),((3,4,8),(3,8,7)))");
        const int left = a.add("TRIANGULATED_FACE(''," + c + ",4,(),$,(),((4,1,5),(4,5,8)))");
        const int solid = a.add(std::format("TESSELLATED_SOLID('',({},{},{},{},{},{}),$)", step_author::ref(bottom),
                                            step_author::ref(top), step_author::ref(front), step_author::ref(right),
                                            step_author::ref(back), step_author::ref(left)));
        a.part("TessellatedCube", {solid}, "TESSELLATED_SHAPE_REPRESENTATION");
        a.color(solid, {0.2, 0.7, 0.7});
        return a.text("tessellated_cube.step");
    }

    // ---- faceted B-rep ----------------------------------------------------
    //
    // A 2 x 1 x 1 box of POLY_LOOP faces, as old AP203 exporters write
    // meshes; some faces plain FACEs, some FACE_SURFACEs on their plane.

    inline std::string faceted_box()
    {
        step_author a;
        const vec3 p[8] = {{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}, {0, 0, 1}, {2, 0, 1}, {2, 1, 1}, {0, 1, 1}};
        int ids[8];
        for (int i = 0; i < 8; ++i)
            ids[i] = a.point(p[i]);
        const int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
        const vec3 normals[6] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}};
        std::string faces;
        for (int f = 0; f < 6; ++f)
        {
            const int loop = a.add(std::format("POLY_LOOP('',({},{},{},{}))", step_author::ref(ids[quads[f][0]]),
                                               step_author::ref(ids[quads[f][1]]), step_author::ref(ids[quads[f][2]]),
                                               step_author::ref(ids[quads[f][3]])));
            const int bound = a.bound(loop);
            int face = 0;
            if (f % 2 == 0)
                face = a.add(std::format("FACE('',({}))", step_author::ref(bound)));
            else
                face = a.add(std::format("FACE_SURFACE('',({}),{},.T.)", step_author::ref(bound),
                                         step_author::ref(a.plane(p[quads[f][0]], normals[f], {1, 0, 0}))));
            faces += (faces.empty() ? "" : ",") + step_author::ref(face);
        }
        const int shell = a.add("CLOSED_SHELL('',(" + faces + "))");
        const int brep = a.add(std::format("FACETED_BREP('',{})", step_author::ref(shell)));
        a.part("FacetedBox", {brep}, "FACETED_BREP_SHAPE_REPRESENTATION");
        return a.text("faceted_box.step");
    }

} // namespace volt::tests::shapes
