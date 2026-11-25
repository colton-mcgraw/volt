// Tests of the private geometry layer: B-spline basis, curves, surfaces and
// their inverses, the polygon triangulation and the face tessellator on
// their own, without a STEP file around them.

#include "../math/check.hpp"

#include "bspline.hpp"
#include "curves.hpp"
#include "face_tessellator.hpp"
#include "surfaces.hpp"
#include "triangulate.hpp"

#include <math/geometry.hpp>

#include <cmath>
#include <cstdio>
#include <numbers>

using namespace volt;
using namespace volt::model::geometry;

namespace
{
    constexpr double pi = std::numbers::pi;

    bool near(double a, double b, double tolerance)
    {
        return std::abs(a - b) <= tolerance;
    }

    double signed_area(const std::vector<math::vec2d> &points, const std::vector<triangle_indices> &triangles)
    {
        double total = 0.0;
        for (const auto &t : triangles)
            total += 0.5 * orient(points[t[0]], points[t[1]], points[t[2]]);
        return total;
    }

    std::size_t flat_triangles(const std::vector<math::vec2d> &points, const std::vector<triangle_indices> &triangles)
    {
        std::size_t count = 0;
        for (const auto &t : triangles)
        {
            if (orient(points[t[0]], points[t[1]], points[t[2]]) <= 1e-12)
                ++count;
        }
        return count;
    }

    void basis()
    {
        // A clamped cubic with one inner knot: partition of unity, and the
        // derivatives sum to zero.
        const bspline_basis b(3, {0, 0, 0, 0, 0.5, 1, 1, 1, 1}, 5);
        CHECK(b.valid());
        for (double t : {0.0, 0.1, 0.5, 0.77, 1.0})
        {
            double values[max_degree + 1];
            double derivatives[max_degree + 1];
            b.evaluate(b.span(t), t, values, derivatives);
            double sum = 0.0;
            double dsum = 0.0;
            for (int i = 0; i <= 3; ++i)
            {
                sum += values[i];
                dsum += derivatives[i];
            }
            CHECK(near(sum, 1.0, 1e-12));
            CHECK(near(dsum, 0.0, 1e-9));
        }
        CHECK(!bspline_basis(3, {0, 0, 1, 1}, 5).valid());
    }

    void inverses()
    {
        model::step::frame frame;
        frame.origin = {1, 2, 3};
        const cone_surface cylinder(frame, 2.0, 0.0);
        const sphere_surface sphere(frame, 1.5);
        const torus_surface torus(frame, 3.0, 1.0);
        const surface *surfaces[] = {&cylinder, &sphere, &torus};
        for (const surface *s : surfaces)
        {
            for (double u = -3.0; u < 3.0; u += 0.7)
            {
                for (double v = -1.2; v < 1.3; v += 0.4)
                {
                    const math::vec3d p = s->point({u, v});
                    const math::vec2d hint{u + 0.01, v};
                    const math::vec2d uv = s->project(p, &hint);
                    CHECK(math::distance(s->point(uv), p) < 1e-9);
                    CHECK(near(uv[0], u, 1e-9)); // unwrapped next to the hint
                }
            }
        }

        // A B-spline curve and Newton's projection onto it.
        const bspline_curve curve(bspline_basis(3, {0, 0, 0, 0, 1, 1, 1, 1}, 4),
                                  {{0, 0, 0}, {1, 2, 0}, {2, -1, 0}, {3, 0, 1}}, {});
        for (double t = 0.0; t <= 1.0; t += 0.125)
            CHECK(near(curve.project(curve.point(t)), t, 1e-9));
    }

    void polygons()
    {
        // A square with a square hole, and many points along straight sides
        // (as sampled edges in parameter space are): every vertex used, the
        // area right, no flat triangles.
        std::vector<math::vec2d> points;
        std::vector<std::vector<std::uint32_t>> rings(2);
        const auto side = [&](std::vector<std::uint32_t> &ring, math::vec2d from, math::vec2d to, int steps) {
            for (int i = 0; i < steps; ++i)
            {
                ring.push_back(static_cast<std::uint32_t>(points.size()));
                points.push_back(from + (to - from) * (static_cast<double>(i) / steps));
            }
        };
        side(rings[0], {0, 0}, {1, 0}, 17);
        side(rings[0], {1, 0}, {1, 1}, 5);
        side(rings[0], {1, 1}, {0, 1}, 9);
        side(rings[0], {0, 1}, {0, 0}, 3);
        side(rings[1], {0.3, 0.3}, {0.3, 0.6}, 4);
        side(rings[1], {0.3, 0.6}, {0.6, 0.6}, 4);
        side(rings[1], {0.6, 0.6}, {0.6, 0.3}, 4);
        side(rings[1], {0.6, 0.3}, {0.3, 0.3}, 4);

        bool clean = true;
        const auto triangles = triangulate_polygon(points, rings, clean);
        CHECK(clean);
        CHECK(near(signed_area(points, triangles), 1.0 - 0.09, 1e-12));
        std::vector<int> used(points.size(), 0);
        for (const auto &t : triangles)
            for (std::uint32_t v : t)
                used[v] = 1;
        bool all = true;
        for (int u : used)
            all = all && u == 1;
        CHECK(all);
        std::printf("polygon: %zu triangles, %zu flat before flips\n", triangles.size(),
                    flat_triangles(points, triangles));

        mesh2d mesh;
        mesh.points = points;
        for (const auto &t : triangles)
            mesh.triangles.push_back({{t[0], t[1], t[2]}, {-1, -1, -1}});
        mesh.connect();
        mesh.make_delaunay();
        std::vector<triangle_indices> after;
        for (const auto &t : mesh.triangles)
            after.push_back({t.v[0], t.v[1], t.v[2]});
        CHECK(near(signed_area(points, after), 1.0 - 0.09, 1e-12));
        CHECK(flat_triangles(points, after) == 0);
    }

    // A sphere's parameter rectangle: long sides of many points (the seam),
    // short ones (the poles) of a few.
    void rectangle()
    {
        std::vector<math::vec2d> points;
        std::vector<std::vector<std::uint32_t>> rings(1);
        const auto side = [&](math::vec2d from, math::vec2d to, int steps) {
            for (int i = 0; i < steps; ++i)
            {
                rings[0].push_back(static_cast<std::uint32_t>(points.size()));
                points.push_back(from + (to - from) * (static_cast<double>(i) / steps));
            }
        };
        const double w = 1.0;
        const double h = 0.62132034355964261;
        side({w, 0}, {w, h}, 14);
        side({w, h}, {0, h}, 2);
        side({0, h}, {0, 0}, 14);
        side({0, 0}, {w, 0}, 2);
        bool clean = true;
        const auto triangles = triangulate_polygon(points, rings, clean);
        mesh2d mesh;
        mesh.points = points;
        for (const auto &t : triangles)
            mesh.triangles.push_back({{t[0], t[1], t[2]}, {-1, -1, -1}});
        mesh.connect();
        std::size_t unlinked = 0;
        for (const auto &t : mesh.triangles)
            for (std::int32_t n : t.n)
                unlinked += n < 0 ? 1 : 0;
        mesh.make_delaunay();
        std::vector<triangle_indices> after;
        for (const auto &t : mesh.triangles)
            after.push_back({t.v[0], t.v[1], t.v[2]});
        std::printf("rectangle: %zu triangles, %zu flat before flips, %zu after, %zu boundary edges (of %zu)\n",
                    triangles.size(), flat_triangles(points, triangles), flat_triangles(points, after), unlinked,
                    points.size());
        CHECK(flat_triangles(points, after) == 0);
        CHECK(unlinked == points.size());
    }

    // The side of an elliptic cylinder, as the STEP test has it.
    void extruded_face()
    {
        model::step::frame frame;
        auto ellipse = std::make_shared<ellipse_curve>(frame, 2.0, 1.0);
        const extrusion_surface surface(ellipse, {0, 0, 3});

        sampling s;
        s.chord = 0.01;
        s.angle = 0.349;
        std::vector<double> ts;
        ellipse->sample(0.0, 2.0 * pi, s, ts);
        std::vector<math::vec3d> loop;
        loop.push_back({2, 0, 0});
        for (double t : ts)
            loop.push_back(ellipse->point(t));
        loop.push_back({2, 0, 0});
        loop.push_back({2, 0, 3});
        for (auto it = ts.rbegin(); it != ts.rend(); ++it)
            loop.push_back(ellipse->point(*it) + math::vec3d{0, 0, 3});
        loop.push_back({2, 0, 3});
        // Every point of the loop projects back onto itself.
        double worst = 0.0;
        math::vec2d hint{};
        for (std::size_t k = 0; k < loop.size(); ++k)
        {
            const math::vec2d uv = surface.project(loop[k], k > 0 ? &hint : nullptr);
            worst = std::max(worst, math::distance(surface.point(uv), loop[k]));
            hint = uv;
        }
        std::printf("extruded face: worst reprojection %.3g\n", worst);
        CHECK(worst < 1e-9);

        face_settings settings;
        settings.chord = 0.01;
        settings.angle = 0.349;
        const auto mesh = tessellate_face(surface, {loop}, true, settings);
        CHECK(mesh.has_value());
        if (!mesh)
        {
            std::printf("extruded face: %s\n", mesh.error().c_str());
            return;
        }
        std::printf("extruded face: %zu points, %zu triangles\n", mesh->positions.size(), mesh->indices.size() / 3);
        CHECK(mesh->indices.size() / 3 < 2000);
    }
    // A whole sphere, one face closed by a seam from pole to pole.
    void sphere_face()
    {
        model::step::frame frame;
        const double r = 1.5;
        const sphere_surface surface(frame, r);
        model::step::frame meridian_frame;
        meridian_frame.z = {0, -1, 0};
        meridian_frame.y = {0, 0, 1};
        const ellipse_curve meridian(meridian_frame, r, r);
        sampling s;
        s.chord = 0.01;
        s.angle = 0.349;
        std::vector<double> ts;
        meridian.sample(-pi / 2, pi / 2, s, ts);
        std::vector<math::vec3d> loop{{0, 0, -r}};
        for (double t : ts)
            loop.push_back(meridian.point(t));
        loop.push_back({0, 0, r});
        for (auto it = ts.rbegin(); it != ts.rend(); ++it)
            loop.push_back(meridian.point(*it));

        face_settings settings;
        settings.chord = 0.01;
        settings.angle = 0.349;
        const auto mesh = tessellate_face(surface, {loop}, true, settings);
        CHECK(mesh.has_value());
        if (!mesh)
            return;
        std::size_t askew = 0;
        for (std::size_t t = 0; t + 2 < mesh->indices.size(); t += 3)
        {
            const std::uint32_t v[3] = {mesh->indices[t], mesh->indices[t + 1], mesh->indices[t + 2]};
            const math::vec3d n = math::cross(mesh->positions[v[1]] - mesh->positions[v[0]],
                                              mesh->positions[v[2]] - mesh->positions[v[0]]);
            const double length = math::magnitude(n);
            if (length < 1e-12)
                continue;
            for (std::uint32_t k : v)
            {
                if (math::dot(n / length, mesh->normals[k]) < std::cos(45.0 * pi / 180.0))
                {
                    ++askew;
                    std::printf("  askew %u %u %u of %zu, uv area %.3g: (%.9g %.9g) (%.9g %.9g) (%.9g %.9g)\n", v[0], v[1],
                                v[2], mesh->positions.size(),
                                orient(mesh->parameters[v[0]], mesh->parameters[v[1]], mesh->parameters[v[2]]),
                                mesh->parameters[v[0]][0],
                                mesh->parameters[v[0]][1], mesh->parameters[v[1]][0], mesh->parameters[v[1]][1],
                                mesh->parameters[v[2]][0], mesh->parameters[v[2]][1]);
                    break;
                }
            }
        }
        std::printf("sphere face: %zu triangles, %zu askew\n", mesh->indices.size() / 3, askew);
        CHECK(askew == 0);
    }
} // namespace

int main()
{
    basis();
    inverses();
    polygons();
    rectangle();
    extruded_face();
    sphere_face();
    return volt::tests::result("model geometry");
}
