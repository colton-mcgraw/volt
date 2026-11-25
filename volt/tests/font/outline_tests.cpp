#include "common.hpp"

#include <font/outline.hpp>
#include <math/geometry.hpp>

#include <cmath>
#include <limits>

using namespace volt::font;
using volt::math::vec2f;
using volt::tests::font::arial;
using volt::tests::font::near;
using volt::tests::font::saira;

namespace
{
    // The area of the flattened polygon, by the shoelace formula.
    double polygon_area(const polygon &shape)
    {
        double twice = 0.0;
        for (std::size_t c = 0; c < shape.contour_count(); ++c)
        {
            const auto points = shape.contour(c);
            for (std::size_t i = 0; i < points.size(); ++i)
            {
                const vec2f a = points[i];
                const vec2f b = points[(i + 1) % points.size()];
                twice += static_cast<double>(a[0] * b[1] - b[0] * a[1]);
            }
        }
        return twice * 0.5;
    }

    void building()
    {
        outline square;
        square.move_to({0.0f, 0.0f});
        square.line_to({10.0f, 0.0f});
        square.line_to({10.0f, 10.0f});
        square.line_to({0.0f, 10.0f});
        square.close(); // adds the line back to the start
        CHECK(square.contour_count() == 1);
        CHECK(square.segments().size() == 4);
        CHECK(square.segments().back().to == vec2f{0.0f, 0.0f});
        CHECK(near(square.signed_area(), 100.0f, 1e-4f)); // counterclockwise, y up

        // A second contour, clockwise: a hole.
        square.move_to({2.0f, 2.0f});
        square.line_to({2.0f, 4.0f});
        square.line_to({4.0f, 4.0f});
        square.line_to({4.0f, 2.0f});
        square.line_to({2.0f, 2.0f}); // already closed: no extra line
        square.close();
        CHECK(square.contour_count() == 2);
        CHECK(square.contour(1).size() == 4);
        CHECK(near(square.signed_area(), 96.0f, 1e-4f));
        CHECK(square.contour(2).empty());

        // Every segment starts where the one before ended.
        for (std::size_t c = 0; c < square.contour_count(); ++c)
        {
            const auto segments = square.contour(c);
            for (std::size_t i = 1; i < segments.size(); ++i)
                CHECK(segments[i].from == segments[i - 1].to);
            CHECK(segments.front().from == segments.back().to);
        }

        // move_to without drawing leaves nothing behind.
        outline nothing;
        nothing.move_to({1.0f, 1.0f});
        nothing.move_to({2.0f, 2.0f});
        nothing.close();
        CHECK(nothing.empty() && nothing.contour_count() == 0);
        CHECK(nothing.control_bounds().empty());
    }

    void curve_areas()
    {
        // The parabola under y = 10 - x^2 / 10 from -10 to 10: area 400 / 3.
        outline parabola;
        parabola.move_to({-10.0f, 0.0f});
        parabola.quadratic_to({0.0f, 20.0f}, {10.0f, 0.0f});
        parabola.close();
        CHECK(near(std::abs(parabola.signed_area()), 400.0f / 3.0f, 1e-3f));

        // A circle of four cubic arcs, radius 10: within the approximation's
        // own error of pi r^2.
        const float k = 0.5522847498f * 10.0f;
        outline circle;
        circle.move_to({10.0f, 0.0f});
        circle.cubic_to({10.0f, k}, {k, 10.0f}, {0.0f, 10.0f});
        circle.cubic_to({-k, 10.0f}, {-10.0f, k}, {-10.0f, 0.0f});
        circle.cubic_to({-10.0f, -k}, {-k, -10.0f}, {0.0f, -10.0f});
        circle.cubic_to({k, -10.0f}, {10.0f, -k}, {10.0f, 0.0f});
        circle.close();
        const float pi = 3.14159265f;
        CHECK(near(circle.signed_area(), 100.0f * pi, 100.0f * pi * 3e-4f));
        // And the closed form agrees with a very fine polygon.
        CHECK(near(circle.signed_area(), static_cast<float>(polygon_area(circle.flatten(1e-4f))), 0.02f));

        // Glyphs: the exact area against a fine polygon, both fonts.
        for (const font &f : {arial(), saira()})
        {
            for (const char32_t c : {U'o', U'g', U'S', U'@', U'&', U'é'})
            {
                const auto shape = f.glyph_outline(f.glyph(c));
                CHECK(shape && !shape->empty());
                if (!shape)
                    continue;
                const auto exact = static_cast<double>(shape->signed_area());
                const double fine = polygon_area(shape->flatten(0.01f));
                CHECK(std::abs(exact - fine) <= std::abs(exact) * 1e-4);
            }
        }
    }

    void bounds()
    {
        // The control point is above the curve: the curve peaks at half its
        // height.
        outline arch;
        arch.move_to({0.0f, 0.0f});
        arch.quadratic_to({5.0f, 10.0f}, {10.0f, 0.0f});
        arch.close();
        CHECK(near(arch.control_bounds().max[1], 10.0f, 1e-6f));
        CHECK(near(arch.bounds().max[1], 5.0f, 1e-5f));
        CHECK(near(arch.bounds().min[0], 0.0f, 1e-6f) && near(arch.bounds().max[0], 10.0f, 1e-6f));

        // A cubic S whose extrema are inside the span.
        outline wave;
        wave.move_to({0.0f, 0.0f});
        wave.cubic_to({0.0f, 10.0f}, {10.0f, -10.0f}, {10.0f, 0.0f});
        wave.close();
        // y(t) = 30 t (1-t)^2 - 30 t^2 (1-t): extremes at t = (3 -+ sqrt 3) / 6.
        const float t = (3.0f - std::sqrt(3.0f)) / 6.0f;
        const float peak = 30.0f * t * (1 - t) * (1 - t) - 30.0f * t * t * (1 - t);
        CHECK(near(wave.bounds().max[1], peak, 1e-4f));
        CHECK(near(wave.bounds().min[1], -peak, 1e-4f));
        CHECK(near(wave.control_bounds().max[1], 10.0f, 1e-6f));
    }

    void flattening()
    {
        const segment quadratic{segment_kind::quadratic, {0.0f, 0.0f}, {50.0f, 100.0f}, {}, {100.0f, 0.0f}};
        const segment cubic{segment_kind::cubic, {0.0f, 0.0f}, {0.0f, 100.0f}, {100.0f, -100.0f}, {100.0f, 0.0f}};
        for (const segment &s : {quadratic, cubic})
        {
            for (const float tolerance : {1.0f, 0.1f, 0.01f})
            {
                const std::uint32_t pieces = subdivisions(s, tolerance);
                CHECK(pieces >= 1 && pieces <= 1024);
                // Midway along each chord, the curve is within tolerance of
                // the chord's midpoint.
                float worst = 0.0f;
                for (std::uint32_t i = 0; i < pieces; ++i)
                {
                    const float t0 = static_cast<float>(i) / static_cast<float>(pieces);
                    const float t1 = static_cast<float>(i + 1) / static_cast<float>(pieces);
                    const vec2f chord = (s.at(t0) + s.at(t1)) * 0.5f;
                    worst = std::max(worst, volt::math::distance(chord, s.at((t0 + t1) * 0.5f)));
                }
                CHECK(worst <= tolerance);
            }
        }
        // Lines are never split; broken curves do not explode.
        CHECK(subdivisions({segment_kind::line, {0, 0}, {}, {}, {1e9f, 1e9f}}, 0.01f) == 1);
        const float infinity = std::numeric_limits<float>::infinity();
        CHECK(subdivisions({segment_kind::quadratic, {0, 0}, {1e30f, 1e30f}, {}, {1, 1}}, 0.01f) == 1024);
        CHECK(subdivisions({segment_kind::quadratic, {0, 0}, {infinity, 0}, {}, {1, 1}}, 0.01f) >= 1);
        CHECK(subdivisions({segment_kind::cubic, {0, 0}, {std::nanf(""), 0}, {0, 0}, {1, 1}}, 0.01f) == 1);

        // The polygon has a contour per contour, and its points on the curve.
        const auto o = arial().glyph_outline(arial().glyph(U'o'));
        CHECK(o.has_value());
        if (o)
        {
            const polygon flat = o->flatten(0.5f);
            CHECK(flat.contour_count() == o->contour_count());
            CHECK(flat.points.size() >= o->segments().size());
            CHECK(flat.contour(0).front() == o->contour(0).front().from);
        }
    }

    void transforms()
    {
        const auto a = arial().glyph_outline(arial().glyph(U'A'));
        CHECK(a.has_value());
        if (!a)
            return;
        // Font units (y up) to 16 px with y down, pen at (100, 50).
        const float scale = arial().scale(16.0f);
        const outline pixels = a->transformed(affine::scale(scale, -scale, {100.0f, 50.0f}));
        const box before = a->control_bounds();
        const box after = pixels.control_bounds();
        CHECK(near(after.min[0], 100.0f + before.min[0] * scale, 1e-3f));
        CHECK(near(after.max[1], 50.0f - before.min[1] * scale, 1e-3f));
        CHECK(near(after.min[1], 50.0f - before.max[1] * scale, 1e-3f));
        // Mirroring flips the sign of the area; scaling multiplies it.
        CHECK(near(pixels.signed_area(), -a->signed_area() * scale * scale, std::abs(a->signed_area()) * 1e-5f));

        outline both = *a;
        both.append(*a, affine::scale(1.0f, 1.0f, {2000.0f, 0.0f}));
        CHECK(both.contour_count() == 2 * a->contour_count());
        CHECK(near(both.signed_area(), 2.0f * a->signed_area(), std::abs(a->signed_area()) * 1e-5f));
        CHECK(near(both.control_bounds().max[0], before.max[0] + 2000.0f, 1e-3f));

        both.clear();
        CHECK(both.empty() && both.contour_count() == 0);
    }
} // namespace

int main()
{
    building();
    curve_areas();
    bounds();
    flattening();
    transforms();
    return volt::tests::result("font outline");
}
