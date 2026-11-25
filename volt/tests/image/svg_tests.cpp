#include "../math/check.hpp"
#include "files.hpp"

#include <image/png.hpp>
#include <image/svg.hpp>
#include <image/xml.hpp>

#include <chrono>
#include <cmath>
#include <string>

using namespace volt;
namespace img = volt::image;
namespace svg = volt::image::svg;
namespace png = volt::image::png;

namespace
{
    bool near(float a, float b, float tolerance = 1e-3f)
    {
        return std::abs(a - b) <= tolerance;
    }

    // ---- XML -------------------------------------------------------------------

    void xml_parsing()
    {
        auto doc = xml::parse("\xEF\xBB\xBF<?xml version=\"1.0\"?>\n"
                              "<!DOCTYPE svg [ <!ENTITY ns \"http://example.com/&#65;\"> <!-- a comment --> ]>\n"
                              "<root a='1' b=\"x &amp; y &lt;&#x41;&#66;&ns;\">\n"
                              "  <!-- skipped -->text<child/>more<![CDATA[<raw>]]>\n"
                              "  <other x=\"tab\there\">&unknown;</other>\n"
                              "</root>\n");
        CHECK(doc.has_value());
        if (doc)
        {
            CHECK(doc->name == "root");
            CHECK(doc->find("a") == "1");
            CHECK(doc->find("b") == "x & y <AB" "http://example.com/A");
            CHECK(!doc->find("c").has_value());
            CHECK(doc->children.size() == 2);
            CHECK(doc->text.find("textmore<raw>") != std::string::npos);
            CHECK(doc->children[1].find("x") == "tab here"); // attribute whitespace normalized
            CHECK(doc->children[1].text == "&unknown;");     // unknown entities kept
            CHECK(doc->children[1].line == 5);
        }

        CHECK(xml::element{"svg:rect", {}, {}, {}, 0}.local_name() == "rect");

        // Errors name the problem and the line.
        auto mismatched = xml::parse("<a>\n<b>\n</a>");
        CHECK(!mismatched && mismatched.error().find("line 3") != std::string::npos);
        CHECK(!xml::parse("").has_value());
        CHECK(!xml::parse("<a>").has_value());
        CHECK(!xml::parse("<a></a><b/>").has_value());
        CHECK(!xml::parse("<a b=c/>").has_value());
        CHECK(!xml::parse("text").has_value());

        // Deep nesting is refused, not a stack overflow.
        std::string deep;
        for (int i = 0; i < 100000; ++i)
            deep += "<g>";
        auto too_deep = xml::parse(deep);
        CHECK(!too_deep && too_deep.error().find("nested") != std::string::npos);

        // Entity expansion is bounded.
        std::string laughs = "<!DOCTYPE x [<!ENTITY a \"" + std::string(1000, 'x') + "\">]><x>";
        for (int i = 0; i < 5000; ++i)
            laughs += "&a;";
        laughs += "</x>";
        CHECK(!xml::parse(laughs).has_value());
    }

    // ---- parsing -----------------------------------------------------------------

    std::string wrap(const std::string &body, const char *size = "width='20' height='20'")
    {
        return std::string("<svg xmlns='http://www.w3.org/2000/svg' ") + size + ">" + body + "</svg>";
    }

    // The shapes of a document, in order.
    std::vector<const svg::shape *> shapes(const svg::document &doc)
    {
        std::vector<const svg::shape *> result;
        for (const svg::item &i : doc.items)
            if (i.type == svg::item::kind::shape)
                result.push_back(&i.shape);
        return result;
    }

    void path_grammar()
    {
        // Absolute and relative, H/V, implicit lineto after moveto, compact
        // numbers ("1.5.5" is 1.5 and .5, "10-5" is 10 and -5).
        auto doc = svg::parse(wrap("<path d='M1.5.5l10-5 h2v3 H0 V0z m1,1 2,2'/>"));
        CHECK(doc.has_value());
        if (doc)
        {
            const auto s = shapes(*doc);
            CHECK(s.size() == 1);
            const auto points = s[0]->path.points();
            const auto verbs = s[0]->path.verbs();
            CHECK(points.size() == 8);
            CHECK(near(points[0][0], 1.5f) && near(points[0][1], 0.5f));
            CHECK(near(points[1][0], 11.5f) && near(points[1][1], -4.5f));
            CHECK(near(points[2][0], 13.5f) && near(points[2][1], -4.5f));
            CHECK(near(points[3][0], 13.5f) && near(points[3][1], -1.5f));
            CHECK(near(points[4][0], 0.0f) && near(points[4][1], -1.5f));
            CHECK(near(points[5][0], 0.0f) && near(points[5][1], 0.0f));
            // After z, m is relative to the subpath's start; then an implicit l.
            CHECK(near(points[6][0], 2.5f) && near(points[6][1], 1.5f));
            CHECK(near(points[7][0], 4.5f) && near(points[7][1], 3.5f));
            CHECK(verbs.size() == 9 && verbs[6] == img::path_verb::close && verbs[7] == img::path_verb::move);
        }

        // Smooth curves reflect the previous control point.
        doc = svg::parse(wrap("<path d='M0 0 C 0 10 10 10 10 0 S 20 -10 20 0 Q 25 5 30 0 T 40 0'/>"));
        if (doc)
        {
            const auto points = shapes(*doc)[0]->path.points();
            CHECK(points.size() == 1 + 3 + 3 + 2 + 2);
            CHECK(near(points[4][0], 10.0f) && near(points[4][1], -10.0f)); // reflected (10, 10) about (10, 0)
            CHECK(near(points[9][0], 35.0f) && near(points[9][1], -5.0f));  // reflected (25, 5) about (30, 0)
        }

        // Arcs: compact flags, and a half circle that ends where it should
        // with its bounds where a circle's are.
        doc = svg::parse(wrap("<path d='M0 0a5 5 0 1010 0'/>"));
        if (doc)
        {
            const auto &p = shapes(*doc)[0]->path;
            CHECK(near(p.current_point()[0], 10.0f) && near(p.current_point()[1], 0.0f));
            const auto box = p.bounds();
            CHECK(near(box.min[0], 0.0f) && near(box.max[0], 10.0f, 1e-2f));
            CHECK(near(box.max[1], 5.0f, 1e-2f) || near(box.min[1], -5.0f, 1e-2f));
        }

        // Radii too small are scaled up; zero radii make a line.
        doc = svg::parse(wrap("<path d='M0 0 A1 1 0 0 1 10 0 M0 5 A0 3 0 0 1 10 5'/>"));
        if (doc)
        {
            const auto &p = shapes(*doc)[0]->path;
            CHECK(near(p.bounds().min[1], -5.0f, 1e-2f));
            CHECK(p.verbs().back() == img::path_verb::line);
        }

        // An error ends the path, but what came before is kept.
        doc = svg::parse(wrap("<path d='M0 0 L10 0 L10 10 X 5 5 L0 10'/>"));
        if (doc)
            CHECK(shapes(*doc)[0]->path.points().size() == 3);
        doc = svg::parse(wrap("<path d='10 10 L 5 5'/>"));
        CHECK(doc && shapes(*doc).empty());
    }

    void shapes_and_style()
    {
        auto doc = svg::parse(wrap(
            "<style>.a { fill: #0f0 } rect.b { stroke: blue; stroke-width: 2 } #c { fill: red } g > rect { fill: pink }"
            "</style>"
            "<rect class='a' width='5' height='5' fill='#123'/>"
            "<rect class='a b' width='5' height='5' rx='1'/>"
            "<rect id='c' class='a' width='5' height='5' style='fill: rgb(0, 0, 255); fill-opacity: 50%'/>"
            "<circle r='3' fill='currentColor' color='hsl(120, 100%, 50%)'/>"
            "<ellipse rx='3' fill='none' stroke='coral' stroke-dasharray='1 2 3' stroke-linecap='round'/>"
            "<line x2='10' stroke='#ff000080' stroke-width='10%'/>"
            "<polygon points='0,0 10,0 10,10' fill-rule='evenodd'/>"
            "<g fill='red' opacity='0.5'><rect width='1' height='1'/><text>skipped</text><unknown/></g>"
            "<rect width='0' height='5'/>"
            "<rect width='5' height='5' display='none'/>"));
        CHECK(doc.has_value());
        if (!doc)
            return;
        const auto s = shapes(*doc);
        CHECK(s.size() == 8);
        if (s.size() != 8)
            return;

        // A sheet rule beats a presentation attribute.
        CHECK(s[0]->fill.type == img::paint::kind::color && near(s[0]->fill.color[1], 1.0f));
        CHECK(s[1]->stroke.type == img::paint::kind::color && near(s[1]->stroke.color[2], 1.0f));
        CHECK(near(s[1]->stroke_style.width, 2.0f));
        CHECK(s[1]->path.verbs().size() > 5); // rounded
        // The style attribute beats the sheet.
        CHECK(near(s[2]->fill.color[2], 1.0f) && near(s[2]->fill.color[0], 0.0f) && near(s[2]->fill_opacity, 0.5f));
        CHECK(near(s[3]->fill.color[1], 1.0f) && near(s[3]->fill.color[0], 0.0f));
        CHECK(s[4]->fill.type == img::paint::kind::none);
        CHECK(s[4]->stroke_style.dashes.size() == 3 && s[4]->stroke_style.cap == img::line_cap::round);
        CHECK(near(s[4]->stroke.color[0], 1.0f) && near(s[4]->stroke.color[1], 127.0f / 255.0f));
        CHECK(near(s[5]->stroke.color[3], 128.0f / 255.0f));
        CHECK(near(s[5]->stroke_style.width, 2.0f)); // 10% of the viewport's normalized diagonal
        CHECK(s[6]->fill_rule == img::fill_rule::even_odd);
        // A rule with a combinator does not apply.
        CHECK(near(s[7]->fill.color[0], 1.0f) && near(s[7]->fill.color[1], 0.0f));
        // The group's opacity became a group.
        int groups = 0;
        for (const svg::item &i : doc->items)
            groups += i.type == svg::item::kind::begin_group;
        CHECK(groups == 1);

        // Sizes: width and height, or the viewBox, or units.
        CHECK(svg::parse(wrap("", "viewBox='0 0 30 10'"))->width == 30.0f);
        CHECK(svg::parse(wrap("", "width='100%' viewBox='0 0 30 10'"))->height == 10.0f);
        CHECK(near(svg::parse(wrap("", "width='1in' viewBox='0 0 30 10'"))->height, 32.0f));
        CHECK(near(svg::parse(wrap("", "width='10mm' height='20pt'"))->width, 37.795f));

        // Not SVG, or not XML.
        CHECK(!svg::parse("<html></html>").has_value());
        CHECK(!svg::parse("<svg><g></svg>").has_value());
    }

    // ---- rasterization -------------------------------------------------------------

    const std::uint8_t *pixel(const img::image &image, std::uint32_t x, std::uint32_t y)
    {
        return image.at(x, y);
    }

    // Area checks flatten finely: at the default tolerance a curve's
    // polygon cuts inside it by up to a tenth of a pixel, which over a
    // circle's perimeter is a pixel or two of area.
    img::image render(const std::string &body, std::uint32_t size = 20, float tolerance = 0.002f)
    {
        auto doc = svg::parse(wrap(body));
        if (!doc)
        {
            std::fprintf(stderr, "%s\n", doc.error().c_str());
            return {};
        }
        svg::rasterize_options options;
        options.width = size;
        options.height = size;
        options.tolerance = tolerance;
        auto result = svg::rasterize(*doc, options);
        return result ? std::move(*result) : img::image{};
    }

    double alpha_sum(const img::image &image)
    {
        double total = 0.0;
        for (std::uint32_t y = 0; y < image.height(); ++y)
            for (std::uint32_t x = 0; x < image.width(); ++x)
                total += image.at(x, y)[3] / 255.0;
        return total;
    }

    void rasterization()
    {
        // A pixel-aligned square is exact.
        img::image square = render("<rect x='5' y='5' width='10' height='10' fill='#f00'/>");
        CHECK(!square.empty());
        if (!square.empty())
        {
            CHECK(pixel(square, 5, 5)[0] == 255 && pixel(square, 5, 5)[3] == 255);
            CHECK(pixel(square, 14, 14)[3] == 255);
            CHECK(pixel(square, 4, 10)[3] == 0 && pixel(square, 15, 10)[3] == 0);
            CHECK(std::abs(alpha_sum(square) - 100.0) < 0.05);
        }

        // Half a pixel off: half covered edges, the same area.
        img::image offset = render("<rect x='5.5' y='5' width='10' height='10'/>");
        CHECK(std::abs(int{pixel(offset, 5, 8)[3]} - 128) <= 1);
        CHECK(std::abs(int{pixel(offset, 15, 8)[3]} - 128) <= 1);
        CHECK(std::abs(alpha_sum(offset) - 100.0) < 0.05);

        // A circle's area, to well under a pixel.
        img::image circle = render("<circle cx='10' cy='10' r='7.3'/>");
        CHECK(std::abs(alpha_sum(circle) - 3.14159265 * 7.3 * 7.3) < 0.05);
        // At the default tolerance, the polygon is inscribed: a little less.
        const double coarse = alpha_sum(render("<circle cx='10' cy='10' r='7.3'/>", 20, 0.1f));
        CHECK(coarse < 3.14159265 * 7.3 * 7.3 && coarse > 3.14159265 * 7.3 * 7.3 - 0.1 * 2.0 * 3.14159265 * 7.3);

        // A diagonal edge: a triangle of half the square.
        img::image triangle = render("<path d='M2 2 L18 2 L18 18 Z'/>");
        CHECK(std::abs(alpha_sum(triangle) - 128.0) < 0.05);
        CHECK(std::abs(int{pixel(triangle, 9, 9)[3]} - 128) <= 1); // on the diagonal

        // Fill rules: a square inside a square, both clockwise.
        const std::string nested = "M2 2 H18 V18 H2 Z M6 6 H14 V14 H6 Z";
        img::image nonzero = render("<path d='" + nested + "'/>");
        img::image even_odd = render("<path fill-rule='evenodd' d='" + nested + "'/>");
        CHECK(pixel(nonzero, 10, 10)[3] == 255);
        CHECK(pixel(even_odd, 10, 10)[3] == 0);
        CHECK(pixel(even_odd, 3, 10)[3] == 255);
        CHECK(std::abs(alpha_sum(even_odd) - (256.0 - 64.0)) < 0.05);

        // Strokes: a 4 wide horizontal line, butt ends, is 4 x 10.
        img::image line = render("<line x1='5' y1='10' x2='15' y2='10' stroke='black' stroke-width='4'/>");
        CHECK(std::abs(alpha_sum(line) - 40.0) < 0.05);
        CHECK(pixel(line, 10, 8)[3] == 255 && pixel(line, 10, 7)[3] == 0);
        // Square caps add half the width at each end.
        img::image capped = render(
            "<line x1='5' y1='10' x2='15' y2='10' stroke='black' stroke-width='4' stroke-linecap='square'/>");
        CHECK(std::abs(alpha_sum(capped) - 56.0) < 0.05);
        // Round caps add a circle.
        img::image rounded = render(
            "<line x1='5' y1='10' x2='15' y2='10' stroke='black' stroke-width='4' stroke-linecap='round'/>");
        CHECK(std::abs(alpha_sum(rounded) - (40.0 + 3.14159265 * 4.0)) < 0.05);
        // A stroked square outline with miter joins: outer 12 minus inner 8.
        img::image outline = render("<rect x='4' y='4' width='10' height='10' fill='none' stroke='black' stroke-width='2'/>");
        CHECK(std::abs(alpha_sum(outline) - (144.0 - 64.0)) < 0.05);
        // Bevel joins cut each corner by a half pixel square.
        img::image beveled = render(
            "<rect x='4' y='4' width='10' height='10' fill='none' stroke='black' stroke-width='2' stroke-linejoin='bevel'/>");
        CHECK(std::abs(alpha_sum(beveled) - (144.0 - 64.0 - 4 * 0.5)) < 0.05);
        // Dashes: 2 on, 2 off along a 12 long line: 3 dashes.
        img::image dashed = render("<line x1='4' y1='10' x2='16' y2='10' stroke='black' stroke-width='2' "
                                   "stroke-dasharray='2'/>");
        CHECK(std::abs(alpha_sum(dashed) - 12.0) < 0.05);
        CHECK(pixel(dashed, 4, 10)[3] == 255 && pixel(dashed, 6, 10)[3] == 0 && pixel(dashed, 8, 10)[3] == 255);
        // A zero-length subpath with round caps is a dot.
        img::image dot = render("<path d='M10 10 z' stroke='black' stroke-width='6' stroke-linecap='round'/>");
        CHECK(std::abs(alpha_sum(dot) - 3.14159265 * 9.0) < 0.05);

        // Opacity: a group's applies once to the overlap, not twice.
        img::image group = render("<g opacity='0.5'><rect width='10' height='10'/><rect width='10' height='10'/></g>");
        CHECK(std::abs(int{pixel(group, 5, 5)[3]} - 128) <= 1);
        img::image overlap = render("<rect width='10' height='10' fill-opacity='0.5'/><rect width='10' height='10' "
                                    "fill-opacity='0.5'/>");
        CHECK(std::abs(int{pixel(overlap, 5, 5)[3]} - 191) <= 1);

        // Transforms, use and symbols.
        img::image moved = render("<defs><rect id='r' width='4' height='4'/></defs>"
                                  "<use href='#r' x='2' y='2'/>"
                                  "<g transform='translate(10 0) scale(2)'><use xlink:href='#r'/></g>");
        CHECK(pixel(moved, 3, 3)[3] == 255 && pixel(moved, 1, 1)[3] == 0);
        CHECK(pixel(moved, 17, 7)[3] == 255 && pixel(moved, 17, 9)[3] == 0);
        img::image symbol = render("<symbol id='s' viewBox='0 0 1 1'><rect width='1' height='1'/></symbol>"
                                   "<use href='#s' x='5' y='5' width='10' height='10'/>");
        CHECK(std::abs(alpha_sum(symbol) - 100.0) < 0.05);
        // A use that refers to itself is ignored, not followed forever.
        img::image loop = render("<g id='g'><use href='#g'/><rect width='2' height='2'/></g>");
        CHECK(pixel(loop, 1, 1)[3] == 255);
    }

    void gradients()
    {
        // Left to right, red to blue, in bounding box units by default.
        img::image linear = render("<linearGradient id='g'><stop offset='0' stop-color='red'/>"
                                   "<stop offset='1' stop-color='blue'/></linearGradient>"
                                   "<rect width='20' height='20' fill='url(#g)'/>");
        CHECK(pixel(linear, 0, 10)[0] > 240 && pixel(linear, 0, 10)[2] < 15);
        CHECK(pixel(linear, 19, 10)[2] > 240 && pixel(linear, 19, 10)[0] < 15);
        CHECK(std::abs(int{pixel(linear, 10, 10)[0]} - int{pixel(linear, 10, 10)[2]}) < 16);

        // Inherited through href; userSpaceOnUse; pad, reflect and repeat.
        img::image spread = render("<linearGradient id='base'><stop offset='0' stop-color='black'/>"
                                   "<stop offset='1' stop-color='white'/></linearGradient>"
                                   "<linearGradient id='g' href='#base' gradientUnits='userSpaceOnUse' x1='0' x2='5' "
                                   "spreadMethod='reflect'/>"
                                   "<rect width='20' height='20' fill='url(#g)'/>");
        CHECK(pixel(spread, 4, 5)[0] > 200);  // near the end: white
        CHECK(pixel(spread, 5, 5)[0] > 200);  // reflected back from white
        CHECK(pixel(spread, 9, 5)[0] < 50);   // back down to black at 10
        img::image repeat = render("<linearGradient id='g' gradientUnits='userSpaceOnUse' x1='0' x2='5' "
                                   "spreadMethod='repeat'><stop offset='0' stop-color='black'/>"
                                   "<stop offset='1' stop-color='white'/></linearGradient>"
                                   "<rect width='20' height='20' fill='url(#g)'/>");
        CHECK(pixel(repeat, 4, 5)[0] > 200 && pixel(repeat, 5, 5)[0] < 50);

        // Radial: white in the center, black at the edge and beyond.
        img::image radial = render("<radialGradient id='g'><stop offset='0' stop-color='white'/>"
                                   "<stop offset='1' stop-color='black'/></radialGradient>"
                                   "<rect width='20' height='20' fill='url(#g)'/>");
        CHECK(pixel(radial, 10, 10)[0] > 230);
        CHECK(pixel(radial, 0, 0)[0] < 10);
        CHECK(std::abs(int{pixel(radial, 15, 10)[0]} - 128) < 20);

        // A missing reference falls back; no stops paints nothing.
        img::image fallback = render("<rect width='20' height='20' fill='url(#missing) lime'/>");
        CHECK(pixel(fallback, 10, 10)[1] == 255);
        img::image empty = render("<linearGradient id='g'/><rect width='20' height='20' fill='url(#g)'/>");
        CHECK(pixel(empty, 10, 10)[3] == 0);
    }

    // ---- the logo --------------------------------------------------------------------

    void logo()
    {
        const auto bytes = tests::read_bytes(tests::asset("volt.svg"));
        CHECK(svg::is_svg(bytes));
        auto doc = svg::parse(bytes);
        CHECK(doc.has_value());
        if (!doc)
        {
            std::fprintf(stderr, "volt.svg: %s\n", doc.error().c_str());
            return;
        }
        CHECK(doc->width == 1080.0f && doc->height == 1080.0f);

        const auto start = std::chrono::steady_clock::now();
        auto rendered = svg::rasterize(*doc);
        const auto took = std::chrono::steady_clock::now() - start;
        CHECK(rendered.has_value());
        if (!rendered)
            return;
        std::printf("volt.svg rasterized at 1080 x 1080 in %.1f ms\n",
                    std::chrono::duration<double, std::milli>(took).count());
        tests::write_bytes(tests::output("volt_svg.png"), *png::encode(*rendered));

        // Against the PNG of the logo, which is on white. That was drawn by
        // another renderer, whose edges are heavier (about 0.78 coverage
        // where exact area gives 0.5), so along every edge some pixels
        // differ a lot; inside, they agree.
        auto reference = png::decode(tests::read_bytes(tests::asset("volt.png")));
        CHECK(reference.has_value());
        if (!reference)
            return;
        double total = 0.0;
        int large = 0;
        for (std::uint32_t y = 0; y < 1080; ++y)
            for (std::uint32_t x = 0; x < 1080; ++x)
            {
                const std::uint8_t *ours = rendered->at(x, y);
                const std::uint8_t *theirs = reference->at(x, y);
                const float a = ours[3] / 255.0f;
                for (int c = 0; c < 3; ++c)
                {
                    const float on_white = ours[c] * a + 255.0f * (1.0f - a);
                    const float d = std::abs(on_white - theirs[c]);
                    total += static_cast<double>(d);
                    large += d > 64.0f;
                }
            }
        const double mean = total / (1080.0 * 1080.0 * 3.0);
        std::printf("volt.svg vs volt.png: mean difference %.2f levels, %d channel values off by more than 64\n", mean,
                    large);
        CHECK(mean < 2.0);
        CHECK(large < 1080 * 1080 * 3 / 100);

        // Smaller, and in linear light, for a look.
        svg::rasterize_options small;
        small.width = 128;
        tests::write_bytes(tests::output("volt_svg_128.png"), *png::encode(*svg::rasterize(*doc, small)));
    }

    // A sheet of features (described in the file), rendered for a person to
    // look at, with a few spot checks.
    void feature_sheet()
    {
        auto doc = svg::parse(tests::read_bytes(tests::asset("tests/svg_features.svg")));
        CHECK(doc.has_value());
        if (!doc)
        {
            std::fprintf(stderr, "svg_features.svg: %s\n", doc.error().c_str());
            return;
        }
        svg::rasterize_options options;
        options.scale = 2.0f;
        auto sheet = svg::rasterize(*doc, options);
        CHECK(sheet.has_value());
        if (!sheet)
            return;
        tests::write_bytes(tests::output("svg_features.png"), *png::encode(*sheet));
        options.linear_light = true;
        tests::write_bytes(tests::output("svg_features_linear.png"), *png::encode(*svg::rasterize(*doc, options)));

        const auto at = [&](float x, float y) { return sheet->at(static_cast<std::uint32_t>(2 * x), static_cast<std::uint32_t>(2 * y)); };
        CHECK(at(50, 55)[3] == 0);                      // the even-odd star's hole
        CHECK(at(150, 55)[3] == 255 && at(150, 55)[0] > 200); // the nonzero star's middle
        CHECK(at(237, 37)[0] > 200 && at(237, 37)[2] > 200);  // near the radial focus: white
        // Group opacity: the overlap is no darker than the rest; separately
        // translucent circles do darken where they overlap.
        CHECK(std::abs(int{at(128, 250)[3]} - int{at(105, 250)[3]}) <= 1);
        CHECK(at(172, 250)[3] > at(197, 250)[3] + 40);
        CHECK(at(342, 250)[3] == 255);                  // the card
        // The pie's slices kept their colors (the combinator rule is ignored).
        CHECK(at(70, 255)[0] > 200 && at(30, 250)[1] > 120);
    }

    void damaged_documents()
    {
        const auto bytes = tests::read_bytes(tests::asset("volt.svg"));
        const std::string text(bytes.begin(), bytes.end());
        tests::xorshift random;
        const char junk[] = "<>/=\"' #-.0123456789eEmMlLzZcCaAsSqQtThHvV(),;:%urlfill";
        int parsed = 0;
        for (int trial = 0; trial < 3000; ++trial)
        {
            std::string damaged = text;
            const int edits = 1 + static_cast<int>(random.next() % 6);
            for (int e = 0; e < edits; ++e)
            {
                const std::size_t at = random.next() % damaged.size();
                switch (random.next() % 3)
                {
                case 0:
                    damaged[at] = junk[random.next() % (sizeof junk - 1)];
                    break;
                case 1:
                    damaged.erase(at, 1 + random.next() % 8);
                    break;
                default:
                    damaged.insert(at, 1, junk[random.next() % (sizeof junk - 1)]);
                    break;
                }
                if (damaged.empty())
                    damaged = "x";
            }
            auto doc = svg::parse(damaged);
            if (!doc)
                continue;
            ++parsed;
            svg::rasterize_options options;
            options.width = 48;
            options.height = 48;
            (void)svg::rasterize(*doc, options);
        }
        std::printf("%d of 3000 damaged copies of volt.svg still parsed (and were drawn)\n", parsed);

        // Hostile numbers draw nothing harmful.
        for (const char *body : {"<path d='M0 0 L1e38 1e38 L-1e38 5 Z'/>", "<circle r='1e30'/>",
                                 "<rect width='1e-30' height='1e-30'/>", "<path d='M0 0 L10 10' stroke='black' "
                                 "stroke-width='1e20'/>",
                                 "<path d='M0 0 L1000 0' stroke='black' stroke-dasharray='0.00001'/>",
                                 "<g transform='scale(0)'><rect width='5' height='5'/></g>",
                                 "<g transform='matrix(1e30 0 0 1e30 0 0)'><rect width='5' height='5'/></g>",
                                 "<path d='M0 0 A 1e-30 1e30 45 1 1 10 10'/>"})
            (void)render(body);

        // A document that claims to be enormous is drawn at a sane size.
        auto vast = svg::parse(wrap("<rect width='10' height='10'/>", "width='1000000' height='500000' viewBox='0 0 20 10'"));
        CHECK(vast.has_value());
        if (vast)
        {
            auto drawn = svg::rasterize(*vast);
            CHECK(drawn && drawn->width() == 4096 && drawn->height() == 2048);
        }

        // Opacity groups nested far deeper than layers are kept still draw.
        std::string nested;
        for (int i = 0; i < 200; ++i)
            nested += "<g opacity='0.99'>";
        nested += "<rect width='20' height='20'/>";
        for (int i = 0; i < 200; ++i)
            nested += "</g>";
        img::image deep = render(nested);
        CHECK(!deep.empty() && deep.at(10, 10)[3] > 200);

        // A <use> bomb stops at the item limit instead of exhausting memory.
        std::string bomb = "<defs><rect id='a0' width='1' height='1'/>";
        for (int level = 1; level <= 8; ++level)
        {
            bomb += "<g id='a" + std::to_string(level) + "'>";
            for (int k = 0; k < 10; ++k)
                bomb += "<use href='#a" + std::to_string(level - 1) + "'/>";
            bomb += "</g>";
        }
        bomb += "</defs><use href='#a8'/>";
        const auto start = std::chrono::steady_clock::now();
        auto doc = svg::parse(wrap(bomb));
        const auto took = std::chrono::steady_clock::now() - start;
        CHECK(doc && doc->items.size() <= 200000 + 10);
        std::printf("a <use> bomb of 10^8 rectangles parsed in %.0f ms into %zu items\n",
                    std::chrono::duration<double, std::milli>(took).count(), doc ? doc->items.size() : 0);
    }
} // namespace

int main()
{
    xml_parsing();
    path_grammar();
    shapes_and_style();
    rasterization();
    gradients();
    logo();
    feature_sheet();
    damaged_documents();
    return volt::tests::result("image svg");
}
