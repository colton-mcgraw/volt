#pragma once

#include "model/step_author.hpp"

#include <math/geometry.hpp>
#include <math/vector.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>
#include <vector>

// The parts on the demo board that have models (assets/models/parts), as
// the STEP files they are: exact B-rep, written the way exporters lay
// them out (step_author), with curved faces where the parts are round, so
// that the tessellator has something to do on screen. Every part has its
// origin at its footprint's origin and z = 0 on the board's top, and is
// in millimeters, as KiCad's library parts are.
//
// demo_parts_tests checks that the committed files are what this makes, and
// that they load clean; `volt_app_demo_parts_tests --write <directory>`
// writes them anew.

namespace volt::tests::demo_parts
{
    using vec2 = math::vec2d;
    using vec3 = math::vec3d;
    inline constexpr double pi = std::numbers::pi;

    // ---- extruded profiles ----------------------------------------------
    //
    // A closed profile in a plane, of straight edges and circular arcs,
    // swept along the plane's normal: planar caps, a planar side for every
    // straight edge and a cylindrical one for every arc.

    struct profile_edge
    {
        vec2 from;        // the edge runs to the next edge's `from`
        bool arc = false; // a circular arc about `centre`
        vec2 centre{};
        bool ccw = true; // which way round the arc runs
    };

    using profile = std::vector<profile_edge>;

    inline double signed_area(const profile &edges)
    {
        // The arcs by their midpoints: enough to tell the turning sense.
        std::vector<vec2> points;
        for (std::size_t i = 0; i < edges.size(); ++i)
        {
            const profile_edge &e = edges[i];
            points.push_back(e.from);
            if (e.arc)
            {
                const vec2 to = edges[(i + 1) % edges.size()].from;
                const double r = math::magnitude(e.from - e.centre);
                double a0 = std::atan2(e.from[1] - e.centre[1], e.from[0] - e.centre[0]);
                double a1 = std::atan2(to[1] - e.centre[1], to[0] - e.centre[0]);
                if (e.ccw && a1 < a0)
                    a1 += 2.0 * pi;
                if (!e.ccw && a1 > a0)
                    a1 -= 2.0 * pi;
                const double mid = 0.5 * (a0 + a1);
                points.push_back(e.centre + vec2{std::cos(mid), std::sin(mid)} * r);
            }
        }
        double area = 0.0;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            const vec2 &a = points[i];
            const vec2 &b = points[(i + 1) % points.size()];
            area += a[0] * b[1] - b[0] * a[1];
        }
        return 0.5 * area;
    }

    // The same outline, run the other way.
    inline profile reversed(const profile &edges)
    {
        profile out;
        const std::size_t n = edges.size();
        for (std::size_t k = 0; k < n; ++k)
        {
            // Edge i ran from point i to point i + 1; reversed, it runs from
            // point i + 1 to point i.
            const std::size_t i = n - 1 - k;
            profile_edge e = edges[i];
            e.from = edges[(i + 1) % n].from;
            e.ccw = !e.ccw;
            out.push_back(e);
        }
        return out;
    }

    inline profile counter_clockwise(const profile &edges)
    {
        return signed_area(edges) >= 0.0 ? edges : reversed(edges);
    }

    // Mirrored across the profile's second axis (u -> -u).
    inline profile mirrored(const profile &edges)
    {
        profile out = edges;
        for (profile_edge &e : out)
        {
            e.from[0] = -e.from[0];
            e.centre[0] = -e.centre[0];
            e.ccw = !e.ccw;
        }
        return out;
    }

    // The solid swept from `outline` (in the plane through `origin` spanned
    // by u and v) along u x v, `length` long.
    inline int extrude(step_author &a, const profile &outline, const vec3 &origin, const vec3 &u, const vec3 &v,
                       double length)
    {
        const profile edges = counter_clockwise(outline);
        const vec3 w = math::cross(u, v);
        const std::size_t n = edges.size();
        const auto at = [&](const vec2 &p, double h) { return origin + u * p[0] + v * p[1] + w * h; };

        std::vector<int> bottom_vertices, top_vertices;
        for (const profile_edge &e : edges)
        {
            bottom_vertices.push_back(a.vertex(at(e.from, 0.0)));
            top_vertices.push_back(a.vertex(at(e.from, length)));
        }

        // Around the outline at the bottom and the top, and up at each corner.
        std::vector<int> bottom, top, up;
        for (std::size_t i = 0; i < n; ++i)
        {
            const profile_edge &e = edges[i];
            const vec2 to = edges[(i + 1) % n].from;
            const std::size_t j = (i + 1) % n;
            for (const double h : {0.0, length})
            {
                const std::vector<int> &vertices = h == 0.0 ? bottom_vertices : top_vertices;
                int curve = 0;
                if (e.arc)
                    curve = a.circle(at(e.centre, h), w, math::normalized(at(e.from, h) - at(e.centre, h)),
                                     math::magnitude(e.from - e.centre));
                else
                    curve = a.line(at(e.from, h), at(to, h));
                // A circle runs counter-clockwise about its axis, w.
                const int edge = a.edge(vertices[i], vertices[j], curve, !e.arc || e.ccw);
                (h == 0.0 ? bottom : top).push_back(edge);
            }
            up.push_back(a.edge(bottom_vertices[i], top_vertices[i], a.line(at(e.from, 0.0), at(e.from, length))));
        }

        std::vector<int> faces;

        std::vector<std::pair<int, bool>> top_loop, bottom_loop;
        for (std::size_t i = 0; i < n; ++i)
        {
            top_loop.push_back({top[i], true});
            bottom_loop.push_back({bottom[n - 1 - i], false});
        }
        faces.push_back(a.face({a.bound(a.loop(top_loop))}, a.plane(at(edges[0].from, length), w, u)));
        faces.push_back(a.face({a.bound(a.loop(bottom_loop))}, a.plane(at(edges[0].from, 0.0), w, u), false));

        for (std::size_t i = 0; i < n; ++i)
        {
            const profile_edge &e = edges[i];
            const std::size_t j = (i + 1) % n;
            const int loop = a.loop({{bottom[i], true}, {up[j], true}, {top[i], false}, {up[i], false}});
            int surface = 0;
            bool sense = true;
            if (e.arc)
            {
                // Outward from the axis where the outline bulges out (an arc
                // that turns the way the outline does), towards it where it
                // is hollow.
                const vec3 x = math::normalized(at(e.from, 0.0) - at(e.centre, 0.0));
                surface = a.add(std::format("CYLINDRICAL_SURFACE('',{},{})",
                                            step_author::ref(a.placement(at(e.centre, 0.0), w, x)),
                                            step_author::real(math::magnitude(e.from - e.centre))));
                sense = e.ccw;
            }
            else
            {
                const vec3 along = math::normalized(at(edges[j].from, 0.0) - at(e.from, 0.0));
                surface = a.plane(at(e.from, 0.0), math::normalized(math::cross(along, w)), along);
            }
            faces.push_back(a.face({a.bound(loop)}, surface, sense));
        }
        return a.solid(faces);
    }

    // A box as an extruded rectangle.
    inline int box(step_author &a, const vec3 &lo, const vec3 &hi)
    {
        const profile rectangle{{{lo[0], lo[1]}}, {{hi[0], lo[1]}}, {{hi[0], hi[1]}}, {{lo[0], hi[1]}}};
        return extrude(a, rectangle, {0.0, 0.0, lo[2]}, {1, 0, 0}, {0, 1, 0}, hi[2] - lo[2]);
    }

    // ---- cylinders ------------------------------------------------------
    //
    // Upright, the lateral face split into sectors by seam lines from
    // `angles` (radians, increasing, at least one), so that a sector can
    // have a colour of its own. Returns the solid and the sectors' faces.

    struct cylinder_parts
    {
        int solid = 0;
        std::vector<int> sectors; // sectors[i] runs from angles[i] to angles[i + 1]
    };

    inline cylinder_parts cylinder(step_author &a, const vec3 &base, double r, double h,
                                   const std::vector<double> &angles = {0.0})
    {
        const vec3 z{0, 0, 1};
        const vec3 x{1, 0, 0};
        const vec3 top = base + z * h;
        const int surface = a.add(std::format("CYLINDRICAL_SURFACE('',{},{})", step_author::ref(a.placement(base, z, x)),
                                              step_author::real(r)));
        const auto rim = [&](const vec3 &centre, double angle) {
            return centre + vec3{std::cos(angle), std::sin(angle), 0.0} * r;
        };

        const std::size_t n = angles.size();
        std::vector<int> vb, vt, seams, eb, et;
        for (double angle : angles)
        {
            vb.push_back(a.vertex(rim(base, angle)));
            vt.push_back(a.vertex(rim(top, angle)));
        }
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::size_t j = (i + 1) % n;
            seams.push_back(a.surface_edge(vb[i], vt[i], a.line(rim(base, angles[i]), rim(top, angles[i])), surface,
                                           n == 1));
            eb.push_back(a.surface_edge(vb[i], vb[j], a.circle(base, z, x, r), surface));
            et.push_back(a.surface_edge(vt[i], vt[j], a.circle(top, z, x, r), surface));
        }

        cylinder_parts parts;
        std::vector<int> faces;
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::size_t j = (i + 1) % n;
            const int face = a.face(
                {a.bound(a.loop({{eb[i], true}, {seams[j], true}, {et[i], false}, {seams[i], false}}))}, surface);
            parts.sectors.push_back(face);
            faces.push_back(face);
        }

        std::vector<std::pair<int, bool>> top_loop, bottom_loop;
        for (std::size_t i = 0; i < n; ++i)
        {
            top_loop.push_back({et[i], true});
            bottom_loop.push_back({eb[n - 1 - i], false});
        }
        faces.push_back(a.face({a.bound(a.loop(top_loop))}, a.plane(top, z)));
        faces.push_back(a.face({a.bound(a.loop(bottom_loop))}, a.plane(base, z), false));
        parts.solid = a.solid(faces);
        return parts;
    }

    // An upright cylinder with a hemisphere on top: an LED's dome.
    inline int domed_cylinder(step_author &a, const vec3 &base, double r, double h)
    {
        const vec3 z{0, 0, 1};
        const vec3 x{1, 0, 0};
        const vec3 top = base + z * h;
        const int side = a.add(std::format("CYLINDRICAL_SURFACE('',{},{})", step_author::ref(a.placement(base, z, x)),
                                           step_author::real(r)));
        const int dome = a.add(std::format("SPHERICAL_SURFACE('',{},{})", step_author::ref(a.placement(top, z, x)),
                                           step_author::real(r)));
        const int vb = a.vertex(base + x * r);
        const int vt = a.vertex(top + x * r);
        const int eb = a.surface_edge(vb, vb, a.circle(base, z, x, r), side);
        const int et = a.surface_edge(vt, vt, a.circle(top, z, x, r), side);
        const int seam = a.surface_edge(vb, vt, a.line(base + x * r, top + x * r), side, true);
        const int lateral = a.face({a.bound(a.loop({{eb, true}, {seam, true}, {et, false}, {seam, false}}))}, side);
        const int head = a.face({a.bound(a.loop({{et, true}}))}, dome);
        const int cap = a.face({a.bound(a.loop({{eb, false}}))}, a.plane(base, z), false);
        return a.solid({lateral, head, cap});
    }

    // ---- the parts ------------------------------------------------------

    // Colours as a datasheet shows them, sRGB.
    inline constexpr vec3 black_body{0.13, 0.13, 0.14};
    inline constexpr vec3 tin{0.80, 0.80, 0.82};
    inline constexpr vec3 marking{0.42, 0.42, 0.44};

    // SOIC-8, 3.9 x 4.9 mm body, 1.27 mm pitch: a moulded body with
    // chamfered long edges and a pin 1 dot, and eight gull-wing leads,
    // each bent through two arcs. Pins run along x, pin 1 at -x, -y.
    inline std::string soic8()
    {
        step_author a;
        std::vector<int> items;

        // The body, chamfered along its long top edges.
        const profile body{{{-1.95, 0.10}}, {{1.95, 0.10}}, {{1.95, 1.40}}, {{1.80, 1.60}},
                           {{-1.80, 1.60}}, {{-1.95, 1.40}}};
        const int moulding = extrude(a, body, {-2.45, 0.0, 0.0}, {0, 1, 0}, {0, 0, 1}, 4.90);
        a.color(moulding, black_body);
        items.push_back(moulding);

        // The pin 1 dot, a shallow dimple's worth above the top.
        const int dot = cylinder(a, {-1.70, -1.00, 1.60}, 0.30, 0.01).solid;
        a.color(dot, marking);
        items.push_back(dot);

        // A lead seen from -x, leaving the body at +y: out, down through a
        // bend, along the board through another. 0.2 mm thick, the bends'
        // centre lines 0.25 mm in radius.
        const profile lead{
            {{1.70, 0.85}},
            {{2.25, 0.85}, true, {2.25, 0.50}, false}, // over the shoulder
            {{2.60, 0.50}},
            {{2.60, 0.35}, true, {2.75, 0.35}, true}, // into the foot, inside
            {{2.75, 0.20}},
            {{3.00, 0.20}},
            {{3.00, 0.00}},
            {{2.75, 0.00}, true, {2.75, 0.35}, false}, // round the heel
            {{2.40, 0.35}},
            {{2.40, 0.50}, true, {2.25, 0.50}, true}, // under the shoulder
            {{2.25, 0.65}},
            {{1.70, 0.65}},
        };
        const double width = 0.41;
        for (int pin = 0; pin < 8; ++pin)
        {
            // 1 to 4 along -y from -x to +x, 5 to 8 back along +y.
            const int column = pin < 4 ? pin : 7 - pin;
            const double x = -1.905 + 1.27 * column - width / 2.0;
            const profile side = pin < 4 ? mirrored(lead) : lead;
            const int solid = extrude(a, side, {x, 0.0, 0.0}, {0, 1, 0}, {0, 0, 1}, width); // along +x
            a.color(solid, tin);
            items.push_back(solid);
        }

        a.part("SOIC-8_3.9x4.9mm_P1.27mm", items);
        return a.text("SOIC-8_3.9x4.9mm_P1.27mm.step");
    }

    // A radial electrolytic capacitor, 6.3 mm across, 7.7 mm tall: a sleeve
    // with the polarity stripe on its negative side as a sector of its own
    // colour, the aluminium can's top, a rubber seal and two leads.
    inline std::string electrolytic()
    {
        step_author a;
        std::vector<int> items;

        const double r = 3.15;
        const double stripe_from = pi - 0.55; // the stripe faces -x, where the negative lead is
        const double stripe_to = pi + 0.55;
        const cylinder_parts sleeve = cylinder(a, {0, 0, 0.30}, r, 7.40, {0.0, stripe_from, stripe_to});
        a.color(sleeve.solid, {0.10, 0.16, 0.42});
        a.color(sleeve.sectors[1], {0.78, 0.82, 0.90});
        items.push_back(sleeve.solid);

        const int can = cylinder(a, {0, 0, 7.70}, 2.85, 0.06).solid;
        a.color(can, {0.82, 0.83, 0.86});
        items.push_back(can);

        const int seal = cylinder(a, {0, 0, 0.0}, 2.90, 0.30).solid;
        a.color(seal, {0.06, 0.06, 0.06});
        items.push_back(seal);

        for (const double x : {-1.25, 1.25})
        {
            const int lead = cylinder(a, {x, 0, -3.5}, 0.25, 3.5).solid;
            a.color(lead, tin);
            items.push_back(lead);
        }

        a.part("CP_Radial_D6.3mm_P2.50mm", items);
        return a.text("CP_Radial_D6.3mm_P2.50mm.step");
    }

    // A 5 mm LED: a red domed body on a flange, and two leads.
    inline std::string led5()
    {
        step_author a;
        std::vector<int> items;

        const int flange = cylinder(a, {0, 0, 0}, 2.90, 1.00).solid;
        a.color(flange, {0.80, 0.08, 0.06});
        items.push_back(flange);

        const int body = domed_cylinder(a, {0, 0, 1.00}, 2.50, 5.10);
        a.color(body, {0.90, 0.12, 0.08});
        items.push_back(body);

        for (const double x : {-1.27, 1.27})
        {
            const int lead = box(a, {x - 0.25, -0.25, -3.5}, {x + 0.25, 0.25, 0.0});
            a.color(lead, tin);
            items.push_back(lead);
        }

        a.part("LED_D5.0mm", items);
        return a.text("LED_D5.0mm.step");
    }

    // A 1206 chip resistor, 3.2 x 1.6 mm: a ceramic body, the black
    // resistive layer on top, and tinned end caps. Along x.
    inline std::string chip_resistor()
    {
        step_author a;
        std::vector<int> items;

        const int body = box(a, {-1.10, -0.80, 0.0}, {1.10, 0.80, 0.50});
        a.color(body, {0.92, 0.92, 0.90});
        items.push_back(body);

        const int layer = box(a, {-1.10, -0.70, 0.50}, {1.10, 0.70, 0.55});
        a.color(layer, {0.08, 0.08, 0.08});
        items.push_back(layer);

        for (const double side : {-1.0, 1.0})
        {
            const double inner = 1.10 * side;
            const double outer = 1.60 * side;
            const int cap = box(a, {std::min(inner, outer), -0.80, 0.0}, {std::max(inner, outer), 0.80, 0.58});
            a.color(cap, tin);
            items.push_back(cap);
        }

        a.part("R_1206_3216Metric", items);
        return a.text("R_1206_3216Metric.step");
    }

    struct part_file
    {
        const char *file;
        std::string (*make)();
    };

    inline constexpr part_file parts[] = {
        {"SOIC-8_3.9x4.9mm_P1.27mm.step", &soic8},
        {"CP_Radial_D6.3mm_P2.50mm.step", &electrolytic},
        {"LED_D5.0mm.step", &led5},
        {"R_1206_3216Metric.step", &chip_resistor},
    };

} // namespace volt::tests::demo_parts
