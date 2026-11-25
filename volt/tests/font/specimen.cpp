// Renders a specimen of the repository's fonts to a PNG, to look at, and
// times the rasterizer. Not a test: nothing is checked here, the eye does.
//
//   volt_font_specimen [output.png]

#include "common.hpp"
#include "png_writer.hpp"

#include <font/atlas.hpp>
#include <font/raster.hpp>
#include <font/text.hpp>

#include <chrono>
#include <cmath>

using namespace volt::font;
using volt::math::vec2f;

namespace
{
    // Ink, 0 to 255, turned into black on white when written.
    struct canvas
    {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> ink;

        canvas(int w, int h) : width(w), height(h), ink(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0) {}

        void put(int x, int y, float value)
        {
            if (x < 0 || y < 0 || x >= width || y >= height)
                return;
            std::uint8_t &target = ink[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                       static_cast<std::size_t>(x)];
            const auto v = static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
            target = std::max(target, v);
        }

        void frame(float x0, float y0, float x1, float y1, float value)
        {
            for (int x = static_cast<int>(x0); x <= static_cast<int>(x1); ++x)
            {
                put(x, static_cast<int>(y0), value);
                put(x, static_cast<int>(y1), value);
            }
            for (int y = static_cast<int>(y0); y <= static_cast<int>(y1); ++y)
            {
                put(static_cast<int>(x0), y, value);
                put(static_cast<int>(x1), y, value);
            }
        }

        bool save(const std::string &path) const
        {
            std::vector<std::uint8_t> gray(ink.size());
            for (std::size_t i = 0; i < ink.size(); ++i)
                gray[i] = static_cast<std::uint8_t>(255 - ink[i]);
            return volt::tests::font::write_png(path, width, height, gray);
        }
    };

    // The UI path: layout, then one atlas quad per glyph.
    float draw_with_atlas(canvas &out, atlas &glyphs, const font_chain &fonts, std::string_view text,
                          const layout_options &options, vec2f origin)
    {
        const text_layout laid = layout(fonts, text, options);
        for (const positioned_glyph &g : laid.glyphs)
        {
            const font &f = fonts.fonts()[g.font];
            const auto quad = glyphs.quad(f, g.glyph, options.size, origin + g.position);
            if (!quad || quad->empty())
                continue;
            const atlas_page page = glyphs.page(quad->page);
            const int u0 = static_cast<int>(std::lround(quad->uv_min[0] * static_cast<float>(page.width)));
            const int v0 = static_cast<int>(std::lround(quad->uv_min[1] * static_cast<float>(page.height)));
            for (int y = 0; y < static_cast<int>(quad->size[1]); ++y)
            {
                for (int x = 0; x < static_cast<int>(quad->size[0]); ++x)
                {
                    const std::uint8_t value = page.pixels[static_cast<std::size_t>(v0 + y) * static_cast<std::size_t>(page.width) +
                                                           static_cast<std::size_t>(u0 + x)];
                    out.put(static_cast<int>(quad->position[0]) + x, static_cast<int>(quad->position[1]) + y,
                            static_cast<float>(value) / 255.0f);
                }
            }
        }
        return laid.extent[1];
    }

    // The geometry path: the layout as one outline, filled.
    float draw_as_outline(canvas &out, const font_chain &fonts, std::string_view text, const layout_options &options,
                          vec2f origin, bool show_box)
    {
        const text_layout laid = layout(fonts, text, options);
        const outline shape = layout_outline(fonts, laid);
        rasterizer fill;
        fill.reset(out.width, out.height);
        fill.add(shape, affine::scale(1.0f, 1.0f, origin));
        std::vector<std::uint8_t> ink(out.ink.size(), 0);
        fill.resolve(ink, static_cast<std::size_t>(out.width));
        for (std::size_t i = 0; i < ink.size(); ++i)
            out.ink[i] = std::max(out.ink[i], ink[i]);
        if (show_box)
            out.frame(origin[0] - 1.0f, origin[1] - 1.0f, origin[0] + options.max_width + 1.0f,
                      origin[1] + laid.extent[1] + 1.0f, 0.3f);
        return laid.extent[1];
    }

    // The zoom path: a distance field made at a small size, drawn large by
    // sampling it bilinearly and thresholding with a pixel-wide smoothstep.
    void draw_field(canvas &out, atlas &fields, const font &f, std::string_view text, float size, vec2f origin)
    {
        const font_chain chain({f});
        layout_options options;
        options.size = size;
        const text_layout laid = layout(chain, text, options);
        const float magnification = size / fields.options().distance_field_size;
        const float spread = fields.options().distance_field_spread;
        for (const positioned_glyph &g : laid.glyphs)
        {
            const auto quad = fields.quad(f, g.glyph, size, origin + g.position);
            if (!quad || quad->empty())
                continue;
            const atlas_page page = fields.page(quad->page);
            const auto sample = [&](float u, float v) {
                const float x = std::clamp(u - 0.5f, 0.0f, static_cast<float>(page.width - 2));
                const float y = std::clamp(v - 0.5f, 0.0f, static_cast<float>(page.height - 2));
                const int ix = static_cast<int>(x);
                const int iy = static_cast<int>(y);
                const float fx = x - static_cast<float>(ix);
                const float fy = y - static_cast<float>(iy);
                const auto at = [&](int px, int py) {
                    return static_cast<float>(page.pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(page.width) +
                                                          static_cast<std::size_t>(px)]) / 255.0f;
                };
                return (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) +
                       (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
            };
            // Half a target pixel either side of the edge, in field units.
            const float width = 0.5f / (2.0f * spread * magnification);
            for (int y = 0; y < static_cast<int>(std::ceil(quad->size[1])); ++y)
            {
                for (int x = 0; x < static_cast<int>(std::ceil(quad->size[0])); ++x)
                {
                    const float u = (quad->uv_min[0] + (static_cast<float>(x) + 0.5f) / quad->size[0] *
                                                           (quad->uv_max[0] - quad->uv_min[0])) *
                                    static_cast<float>(page.width);
                    const float v = (quad->uv_min[1] + (static_cast<float>(y) + 0.5f) / quad->size[1] *
                                                           (quad->uv_max[1] - quad->uv_min[1])) *
                                    static_cast<float>(page.height);
                    const float d = sample(u, v);
                    const float t = std::clamp((d - (0.5f - width)) / (2.0f * width), 0.0f, 1.0f);
                    out.put(static_cast<int>(quad->position[0]) + x, static_cast<int>(quad->position[1]) + y,
                            t * t * (3.0f - 2.0f * t));
                }
            }
        }
    }

    template <typename F>
    double milliseconds(F &&work)
    {
        const auto start = std::chrono::steady_clock::now();
        work();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void timings()
    {
        const font arial = volt::tests::font::arial();
        std::vector<glyph_id> ascii;
        for (char32_t c = 0x21; c < 0x7F; ++c)
            ascii.push_back(arial.glyph(c));

        std::vector<outline> outlines;
        const double parse = milliseconds([&] {
            for (int round = 0; round < 10; ++round)
            {
                outlines.clear();
                for (const glyph_id g : ascii)
                    outlines.push_back(*arial.glyph_outline(g));
            }
        });
        std::printf("outlines: %.2f us per glyph (94 ASCII glyphs x 10)\n", parse * 1000.0 / (10.0 * 94.0));

        for (const float size : {12.0f, 16.0f, 32.0f, 96.0f, 256.0f})
        {
            std::size_t pixels = 0;
            const int rounds = size > 100.0f ? 5 : 50;
            const double time = milliseconds([&] {
                for (int round = 0; round < rounds; ++round)
                {
                    for (const outline &shape : outlines)
                        pixels += rasterize(shape, arial.scale(size))->pixels.size();
                }
            });
            std::printf("coverage %5.0f px: %7.2f us per glyph, %.1f ns per pixel\n", static_cast<double>(size),
                        time * 1000.0 / (rounds * 94.0), time * 1e6 / static_cast<double>(pixels));
        }
        for (const float size : {32.0f, 48.0f, 64.0f})
        {
            const double time = milliseconds([&] {
                for (const outline &shape : outlines)
                    (void)distance_field(shape, arial.scale(size), size / 8.0f);
            });
            std::printf("distance field %2.0f px (spread %.0f): %.2f ms per glyph\n", static_cast<double>(size),
                        static_cast<double>(size / 8.0f), time / 94.0);
        }

        const font_chain fonts({arial});
        std::string paragraph;
        for (int i = 0; i < 100; ++i)
            paragraph += "The quick brown fox jumps over the lazy dog. ";
        layout_options options;
        options.max_width = 400.0f;
        const double laid = milliseconds([&] {
            for (int i = 0; i < 20; ++i)
                (void)layout(fonts, paragraph, options);
        });
        std::printf("layout: %.3f ms for %zu bytes wrapped at 400 px\n", laid / 20.0, paragraph.size());

        atlas glyphs;
        const double filled = milliseconds([&] {
            for (const float size : {12.0f, 16.0f, 24.0f})
                for (const glyph_id g : ascii)
                    for (const float pen : {0.0f, 0.25f, 0.5f, 0.75f})
                        (void)glyphs.glyph(arial, g, size, pen);
        });
        std::printf("atlas: %.2f us per new entry (%zu entries)\n", filled * 1000.0 / static_cast<double>(glyphs.glyph_count()),
                    glyphs.glyph_count());
    }
} // namespace

int main(int argc, char **argv)
{
    const std::string path = argc > 1 ? argv[1] : "font_specimen.png";
    const font arial = volt::tests::font::arial();
    const font saira = volt::tests::font::saira();

    canvas out(1400, 1500);
    atlas glyphs;
    float y = 12.0f;

    // 1. The atlas path, both fonts, at the four sizes.
    const char *pangram = "The quick brown fox jumps over the lazy dog. 0123456789 AV To Wa — Ω ≈ ∞ "
                          "€ é Å (fi) [x] {y} @#%&";
    for (const font &f : {arial, saira})
    {
        const font_chain chain({f});
        for (const float size : {12.0f, 16.0f, 32.0f, 96.0f})
        {
            layout_options options;
            options.size = size;
            options.max_width = 1380.0f;
            options.wrap = false;
            const char *text = size > 50.0f ? "Volt AVATAR 12.5 kΩ Åg" : pangram;
            y += draw_with_atlas(out, glyphs, chain, text, options, {10.0f, y});
        }
        y += 8.0f;
    }

    // 2. Geometry: wrapping in a box, three alignments, ellipsis at three
    // lines; and a fallback chain.
    const char *paragraph = "Schematic and board text is geometry: these lines were laid out at 18 px, "
                            "wrapped at 400 px, filled as one outline. A very-long-hyphenated-word wraps too, "
                            "and what does not fit in three lines ends in an ellipsis.";
    float tallest = 0.0f;
    float x = 10.0f;
    for (const text_align align : {text_align::left, text_align::center, text_align::right})
    {
        layout_options options;
        options.size = 18.0f;
        options.max_width = 400.0f;
        options.align = align;
        options.max_lines = align == text_align::right ? 3 : 0;
        tallest = std::max(tallest, draw_as_outline(out, font_chain({arial}), paragraph, options, {x, y + 4.0f}, true));
        x += 460.0f;
    }
    y += tallest + 16.0f;

    layout_options fallback;
    fallback.size = 28.0f;
    y += draw_with_atlas(out, glyphs, font_chain({saira, arial}),
                         "Saira, falling back to Arial: Жизнь שלום "
                         "αβγ → 一 (none has the last)",
                         fallback, {10.0f, y + 4.0f}) +
         12.0f;

    // 3. Zoom: a distance field made at 32 px drawn at 200 px, beside
    // coverage rasterized at 200 px.
    atlas fields({.content = atlas_content::distance_field, .distance_field_size = 32.0f,
                  .distance_field_spread = 4.0f});
    draw_field(out, fields, arial, "Rg&", 200.0f, {10.0f, y});
    layout_options big;
    big.size = 200.0f;
    y += draw_with_atlas(out, glyphs, font_chain({arial}), "Rg&", big, {700.0f, y});

    std::printf("specimen: %s, %d x %d, used to y %.0f\n", path.c_str(), out.width, out.height,
                static_cast<double>(y));
    if (!out.save(path))
    {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }

    timings();
    return 0;
}
