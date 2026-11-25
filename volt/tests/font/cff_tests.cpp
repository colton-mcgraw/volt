#include "cff_fixture.hpp"
#include "common.hpp"
#include "exercise.hpp"

#include <font/raster.hpp>
#include <font/text.hpp>

#include <numeric>
#include <random>

// CFF outlines, against a fixture made by make_cff_fixture.py: Arial's
// glyphs as cubics, with subroutines, hints, the width operand and every
// flex operator. The expected areas and boxes are fontTools'.

using namespace volt::font;
using volt::tests::font::cff_fixture;
using volt::tests::font::cff_fixture_glyphs;
using volt::tests::font::near;

namespace
{
    std::vector<std::byte> fixture_bytes()
    {
        std::vector<std::byte> bytes(sizeof(cff_fixture));
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::byte>(cff_fixture[i]);
        return bytes;
    }

    font load_fixture()
    {
        auto loaded = font::load(fixture_bytes());
        if (!loaded)
        {
            std::fprintf(stderr, "cannot load the CFF fixture: %s\n", loaded.error().c_str());
            std::exit(1);
        }
        return *loaded;
    }

    void tables(const font &f)
    {
        CHECK(f.format() == outline_format::cff);
        CHECK(f.units_per_em() == 2048);
        CHECK(f.glyph_count() == 8);
        CHECK(f.family() == "Volt CFF Fixture");
        CHECK(f.metrics().line == line_metrics{1854, -434, 0});
        CHECK(!f.has_kerning());
    }

    void outlines(const font &f)
    {
        for (const auto &expected : cff_fixture_glyphs)
        {
            if (expected.code_point != 0)
                CHECK(f.glyph(expected.code_point) == expected.glyph);
            CHECK(f.advance(expected.glyph) == expected.advance);

            const auto shape = f.glyph_outline(expected.glyph);
            CHECK(shape.has_value());
            if (!shape)
            {
                std::fprintf(stderr, "  %s: %s\n", expected.name, shape.error().c_str());
                continue;
            }
            const double area = static_cast<double>(shape->signed_area());
            const bool area_ok = std::abs(area - expected.area) <= std::abs(expected.area) * 1e-5 + 0.5;
            const box control = shape->control_bounds();
            const box tight = shape->bounds();
            const bool control_ok = near(control.min[0], expected.control[0], 0.01f) &&
                                    near(control.min[1], expected.control[1], 0.01f) &&
                                    near(control.max[0], expected.control[2], 0.01f) &&
                                    near(control.max[1], expected.control[3], 0.01f);
            const bool tight_ok = near(tight.min[0], expected.tight[0], 0.05f) &&
                                  near(tight.min[1], expected.tight[1], 0.05f) &&
                                  near(tight.max[0], expected.tight[2], 0.05f) &&
                                  near(tight.max[1], expected.tight[3], 0.05f);
            if (!area_ok || !control_ok || !tight_ok)
                std::fprintf(stderr, "  %s: area %g (fontTools %g), box %g %g %g %g\n", expected.name, area,
                             expected.area, static_cast<double>(control.min[0]), static_cast<double>(control.min[1]),
                             static_cast<double>(control.max[0]), static_cast<double>(control.max[1]));
            CHECK(area_ok);
            CHECK(control_ok);
            CHECK(tight_ok);

            // Every curve is a cubic or a line; contours are closed.
            for (const segment &s : shape->segments())
                CHECK(s.kind != segment_kind::quadratic);
            for (std::size_t c = 0; c < shape->contour_count(); ++c)
                CHECK(shape->contour(c).front().from == shape->contour(c).back().to);

            // A CFF glyph's box is its outline's.
            CHECK(f.metrics(expected.glyph).bounds == tight);
        }
    }

    // The same glyph as cubics and as quadratics covers the same area, up to
    // the rounding of the converted coordinates to whole units.
    void against_truetype(const font &f)
    {
        const font arial = volt::tests::font::arial();
        for (const char32_t c : {U'A', U'g', U'o', U'&', U'é'})
        {
            const auto cubic = f.glyph_outline(f.glyph(c));
            const auto quadratic = arial.glyph_outline(arial.glyph(c));
            CHECK(cubic && quadratic);
            if (cubic && quadratic)
                CHECK(std::abs(cubic->signed_area() - quadratic->signed_area()) <=
                      std::abs(quadratic->signed_area()) * 2e-3f);
        }

        // And rasterizes the same.
        const auto image = rasterize(f, f.glyph(U'&'), 64.0f);
        const auto reference = rasterize(arial, arial.glyph(U'&'), 64.0f);
        CHECK(image && reference && image->width == reference->width && image->offset == reference->offset);
        if (image && reference && image->pixels.size() == reference->pixels.size())
        {
            int worst = 0;
            for (std::size_t i = 0; i < image->pixels.size(); ++i)
                worst = std::max(worst, std::abs(int{image->pixels[i]} - int{reference->pixels[i]}));
            CHECK(worst <= 8); // a 64 px glyph moved by under a sixtieth of a pixel
        }

        const font_chain chain({f});
        CHECK(near(measure(chain, "Ag", 2048.0f), 1366.0f + 1139.0f, 1e-3f));
    }

    void corruption()
    {
        const std::vector<std::byte> original = fixture_bytes();
        std::size_t loaded = 0;
        std::size_t copies = 0;
        // Every truncation.
        for (std::size_t length = 0; length < original.size(); ++length)
        {
            const std::vector<std::byte> damaged(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(length));
            if (const auto f = font::load(damaged))
            {
                ++loaded;
                (void)volt::tests::font::exercise(*f, 1);
            }
            ++copies;
        }
        // Bit flips, concentrated on the CFF table (the bytes after the
        // sfnt tables come last in the fixture, so: the second half).
        std::mt19937 random(7);
        for (int round = 0; round < 3000; ++round)
        {
            std::vector<std::byte> damaged = original;
            const int flips = 1 + round % 6;
            for (int flip = 0; flip < flips; ++flip)
            {
                const std::size_t at = round % 4 == 0 ? random() % damaged.size()
                                                      : damaged.size() / 3 + random() % (damaged.size() * 2 / 3);
                damaged[at] ^= static_cast<std::byte>(1u << (random() % 8));
            }
            if (const auto f = font::load(damaged))
            {
                ++loaded;
                (void)volt::tests::font::exercise(*f, 1);
            }
            ++copies;
        }
        std::printf("  %zu damaged copies of the CFF fixture, %zu loaded anyway, all returned\n", copies, loaded);
    }
} // namespace

int main()
{
    const font f = load_fixture();
    tables(f);
    outlines(f);
    against_truetype(f);
    corruption();
    return volt::tests::result("font cff");
}
