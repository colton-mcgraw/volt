#include "common.hpp"

#include <font/raster.hpp>

#include <cmath>
#include <numeric>

using namespace volt::font;
using volt::math::vec2f;
using volt::tests::font::arial;
using volt::tests::font::near;
using volt::tests::font::saira;

namespace
{
    outline rectangle(float x0, float y0, float x1, float y1)
    {
        outline shape;
        shape.move_to({x0, y0});
        shape.line_to({x1, y0});
        shape.line_to({x1, y1});
        shape.line_to({x0, y1});
        shape.close();
        return shape;
    }

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

    double coverage_sum(const rasterizer &canvas)
    {
        std::vector<float> cells(static_cast<std::size_t>(canvas.width() * canvas.height()));
        canvas.resolve(cells);
        return std::accumulate(cells.begin(), cells.end(), 0.0);
    }

    // Pixel-aligned shapes come out exact.
    void aligned()
    {
        rasterizer canvas;
        canvas.reset(8, 6);
        canvas.add(rectangle(2, 1, 5, 4), {});
        std::vector<std::uint8_t> pixels(48, 0);
        canvas.resolve(pixels, 8);
        for (int y = 0; y < 6; ++y)
        {
            for (int x = 0; x < 8; ++x)
            {
                const bool inside = x >= 2 && x < 5 && y >= 1 && y < 4;
                CHECK(pixels[static_cast<std::size_t>(y * 8 + x)] == (inside ? 255 : 0));
            }
        }

        // Half a pixel off in both directions: halves on the sides, quarters
        // in the corners, and the total area exact.
        canvas.reset(8, 8);
        canvas.add(rectangle(1.5f, 1.5f, 4.5f, 5.5f), {});
        std::vector<float> cells(64);
        canvas.resolve(cells);
        CHECK(near(cells[1 * 8 + 1], 0.25f, 1e-5f));
        CHECK(near(cells[1 * 8 + 2], 0.5f, 1e-5f));
        CHECK(near(cells[3 * 8 + 1], 0.5f, 1e-5f));
        CHECK(near(cells[3 * 8 + 3], 1.0f, 1e-5f));
        CHECK(near(cells[5 * 8 + 4], 0.25f, 1e-5f));
        CHECK(near(static_cast<float>(coverage_sum(canvas)), 12.0f, 1e-4f));

        // Either winding direction gives the same coverage.
        canvas.reset(8, 8);
        canvas.add(rectangle(1.5f, 1.5f, 4.5f, 5.5f), affine::scale(1.0f, -1.0f, {0.0f, 7.0f}));
        CHECK(near(static_cast<float>(coverage_sum(canvas)), 12.0f, 1e-4f));
    }

    // A slanted triangle crossing many cells: area exact.
    void triangles()
    {
        rasterizer canvas;
        canvas.reset(40, 30);
        outline triangle;
        triangle.move_to({1.3f, 2.7f});
        triangle.line_to({37.9f, 9.1f});
        triangle.line_to({12.2f, 28.6f});
        triangle.close();
        canvas.add(triangle, {});
        CHECK(near(static_cast<float>(coverage_sum(canvas)), std::abs(triangle.signed_area()), 1e-2f));

        // Parts outside the canvas are cut off, not wrapped or crashed on.
        canvas.reset(10, 10);
        canvas.add(rectangle(-5, -5, 5, 5), {});
        CHECK(near(static_cast<float>(coverage_sum(canvas)), 25.0f, 1e-3f));
        canvas.reset(10, 10);
        canvas.add(rectangle(-1e6f, 3, 1e6f, 4), {});
        CHECK(near(static_cast<float>(coverage_sum(canvas)), 10.0f, 1e-2f));
        canvas.reset(10, 10);
        canvas.line({std::nanf(""), 0}, {1, 1});
        canvas.reset(0, 0);
        canvas.add(rectangle(0, 0, 1, 1), {});
    }

    // Glyphs: the coverage adds up to the area of the polygon drawn, and
    // that to the area of the outline.
    void glyph_area(const font &f, char32_t code_point, float pixel_size)
    {
        const auto shape = f.glyph_outline(f.glyph(code_point));
        CHECK(shape.has_value());
        if (!shape)
            return;
        const float scale = f.scale(pixel_size);
        const auto image = rasterize(*shape, scale, 0.25f);
        CHECK(image.has_value());
        if (!image)
            return;

        rasterizer canvas;
        canvas.reset(image->width, image->height);
        const affine placement = affine::scale(
            scale, -scale, {0.25f - static_cast<float>(image->offset[0]), -static_cast<float>(image->offset[1])});
        canvas.add(*shape, placement);
        const double drawn = coverage_sum(canvas);
        const double polygon = std::abs(polygon_area(shape->transformed(placement).flatten(raster_tolerance)));
        const double exact =
            std::abs(static_cast<double>(shape->signed_area())) * static_cast<double>(scale * scale);
        CHECK(std::abs(drawn - polygon) <= polygon * 1e-4 + 1e-3);
        if (std::abs(drawn - exact) > exact * 0.01)
            std::fprintf(stderr, "%s U+%04X %g px: drawn %g exact %g polygon %g\n", f.family().c_str(),
                         static_cast<unsigned>(code_point), static_cast<double>(pixel_size), drawn, exact, polygon);
        CHECK(std::abs(drawn - exact) <= exact * 0.01);

        // The bytes agree with the fractions, to rounding.
        double bytes = 0.0;
        for (const std::uint8_t value : image->pixels)
            bytes += value / 255.0;
        CHECK(std::abs(bytes - drawn) <= 0.5 / 255.0 * static_cast<double>(image->pixels.size()) + drawn * 1e-3);
    }

    void glyph_images()
    {
        for (const float size : {12.0f, 16.0f, 32.0f, 96.0f})
        {
            for (const char32_t c : {U'o', U'g', U'A', U'S', U'@', U'%'})
            {
                glyph_area(arial(), c, size);
                glyph_area(saira(), c, size);
            }
        }

        // Arial's A at 20.48 px (0.01 px per unit): its box (-3, 0, 1369,
        // 1466) covers x -0.03 to 13.69 and y -14.66 to 0.
        const auto a = rasterize(arial(), arial().glyph(U'A'), 20.48f);
        CHECK(a.has_value());
        if (a)
        {
            CHECK(a->offset == volt::math::vec2i{-1, -15});
            CHECK(a->width == 15 && a->height == 15);
            CHECK(a->pixels.size() == 225u);
            // The apex is at the top, the feet at the bottom row.
            int bottom = 0;
            for (int x = 0; x < a->width; ++x)
                bottom += a->at(x, a->height - 1);
            CHECK(bottom > 0);
        }

        // A subpixel shift moves the ink exactly. Arial's l is the rectangle
        // x 131 to 311, y 0 to 1466: every column's coverage is the share of
        // it the shifted rectangle spans.
        for (const float shift : {0.0f, 0.25f, 0.5f, 0.75f})
        {
            const float scale = arial().scale(32.0f);
            const auto image = rasterize(arial(), arial().glyph(U'l'), 32.0f, shift);
            CHECK(image.has_value());
            if (!image)
                continue;
            const float left = 131.0f * scale + shift;
            const float right = 311.0f * scale + shift;
            CHECK(image->offset[0] == static_cast<int>(std::floor(left)));
            CHECK(image->width == static_cast<int>(std::ceil(right)) - image->offset[0]);
            for (int x = 0; x < image->width; ++x)
            {
                const float column = static_cast<float>(x + image->offset[0]);
                const float share = std::clamp(std::min(column + 1.0f, right) - std::max(column, left), 0.0f, 1.0f);
                CHECK(std::abs(static_cast<float>(image->at(x, image->height / 2)) - share * 255.0f) <= 0.51f);
            }
        }

        // Blank glyphs make empty images; absurd sizes are refused.
        const auto space = rasterize(arial(), arial().glyph(U' '), 32.0f);
        CHECK(space && space->empty());
        CHECK(!rasterize(arial(), arial().glyph(U'W'), 100000.0f));
        CHECK(!rasterize(arial(), 60000, 32.0f));
        const auto zero = rasterize(arial(), arial().glyph(U'W'), 0.0f);
        CHECK(zero && zero->empty());
    }

    void distance_fields()
    {
        // A 20 x 20 square, spread 4: the field's value at each pixel center
        // is its signed distance to the nearest side.
        const outline square = rectangle(0, 0, 20, 20);
        const auto field = distance_field(square, 1.0f, 4.0f);
        CHECK(field.has_value());
        if (!field)
            return;
        CHECK(field->width == 28 && field->height == 28);
        CHECK(field->offset == volt::math::vec2i{-4, -24});
        for (int y = 0; y < field->height; ++y)
        {
            for (int x = 0; x < field->width; ++x)
            {
                // Pixel center in the square's frame (y up).
                const float px = static_cast<float>(x) + 0.5f - 4.0f;
                const float py = 20.0f - (static_cast<float>(y) + 0.5f - 4.0f);
                const bool inside = px > 0 && px < 20 && py > 0 && py < 20;
                float distance = 0.0f;
                if (inside)
                    distance = std::min({px, 20 - px, py, 20 - py});
                else
                {
                    const float dx = std::max({-px, px - 20, 0.0f});
                    const float dy = std::max({-py, py - 20, 0.0f});
                    distance = -std::sqrt(dx * dx + dy * dy);
                }
                const float expected = std::clamp(0.5f + distance / 8.0f, 0.0f, 1.0f) * 255.0f;
                CHECK(std::abs(static_cast<float>(field->at(x, y)) - expected) <= 0.6f);
            }
        }

        // On a glyph: inside the field means ink in the coverage image.
        const glyph_id g = arial().glyph(U'g');
        const auto sdf = distance_field(arial(), g, 48.0f, 6.0f);
        const auto ink = rasterize(arial(), g, 48.0f);
        CHECK(sdf && ink);
        if (sdf && ink)
        {
            int disagreements = 0;
            for (int y = 0; y < sdf->height; ++y)
            {
                for (int x = 0; x < sdf->width; ++x)
                {
                    const int ix = x + sdf->offset[0] - ink->offset[0];
                    const int iy = y + sdf->offset[1] - ink->offset[1];
                    const bool has_ink = ix >= 0 && iy >= 0 && ix < ink->width && iy < ink->height &&
                                         ink->at(ix, iy) > 0;
                    const bool deep_inside = sdf->at(x, y) > 128 + 255 / 12; // half a pixel in
                    const bool far_outside = sdf->at(x, y) < 128 - 255 / 12;
                    if ((deep_inside && !has_ink) || (far_outside && has_ink && ink->at(ix, iy) == 255))
                        ++disagreements;
                }
            }
            CHECK(disagreements == 0);
        }

        CHECK(!distance_field(square, 1.0f, 0.0f));
        CHECK(!distance_field(square, 1.0f, -1.0f));
        const auto blank = distance_field(arial(), arial().glyph(U' '), 32.0f, 4.0f);
        CHECK(blank && blank->empty());
    }
} // namespace

int main()
{
    aligned();
    triangles();
    glyph_images();
    distance_fields();
    return volt::tests::result("font raster");
}
