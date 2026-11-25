// The stroke font: every glyph is drawn and stays inside its box; widths
// add up and scale with the height; alignment puts the text where it says
// in all four turns, and turned text is never upside down or mirrored;
// UTF-8 is decoded, the symbols of values included, and bad UTF-8 draws
// boxes instead of failing.

#include "../math/check.hpp"

#include <schematic/stroke_font.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace volt;
using namespace volt::schematic;

namespace
{
    constexpr float epsilon = 1e-4f;

    bool near(float a, float b, float tolerance = epsilon)
    {
        return std::fabs(a - b) <= tolerance;
    }

    bool same(const segment &x, const segment &y, float tolerance = epsilon)
    {
        return near(x.a.x(), y.a.x(), tolerance) && near(x.a.y(), y.a.y(), tolerance) &&
               near(x.b.x(), y.b.x(), tolerance) && near(x.b.y(), y.b.y(), tolerance) && x.width == y.width &&
               x.color == y.color && x.node == y.node;
    }

    bool same(const std::vector<segment> &x, const std::vector<segment> &y)
    {
        if (x.size() != y.size())
            return false;
        for (std::size_t i = 0; i < x.size(); ++i)
            if (!same(x[i], y[i]))
                return false;
        return true;
    }

    struct extent
    {
        float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    };

    // The extent of the segments' end points (widths left out).
    extent extent_of(const std::vector<segment> &segments)
    {
        extent e;
        for (const segment &s : segments)
            for (const math::vec2f p : {s.a, s.b})
            {
                e.min_x = std::min(e.min_x, p.x());
                e.min_y = std::min(e.min_y, p.y());
                e.max_x = std::max(e.max_x, p.x());
                e.max_y = std::max(e.max_y, p.y());
            }
        return e;
    }

    std::vector<segment> draw(std::string_view text, math::vec2f anchor, float height, horizontal_align h,
                              vertical_align v, int turns)
    {
        std::vector<segment> out;
        append_text(out, text, anchor, height, h, v, turns, 0.1f, math::vec4f{1.0f, 0.5f, 0.25f, 1.0f});
        return out;
    }

    std::vector<char32_t> covered()
    {
        std::vector<char32_t> all;
        for (char32_t c = 32; c <= 126; ++c)
            all.push_back(c);
        for (const char32_t c : {U'Ω', U'µ', U'μ', U'°', U'±'})
            all.push_back(c);
        return all;
    }

    void glyphs()
    {
        const stroke_glyph &unknown = find_glyph(U'一');
        for (const char32_t c : covered())
        {
            CHECK(has_glyph(c));
            const stroke_glyph &g = find_glyph(c);
            CHECK(g.advance > 0.0f && g.advance < 1.5f);
            if (c == U' ')
            {
                CHECK(g.strokes.empty());
                continue;
            }
            CHECK(!g.strokes.empty());
            CHECK(&g != &unknown);
            bool inside = true;
            for (const auto &stroke : g.strokes)
            {
                inside = inside && !stroke.empty();
                for (const math::vec2f p : stroke)
                    inside = inside && std::isfinite(p.x()) && std::isfinite(p.y()) && p.x() >= -epsilon &&
                             p.x() <= g.advance + epsilon && p.y() >= -font_descent - epsilon &&
                             p.y() <= font_ascent + epsilon;
            }
            CHECK(inside);
            if (!inside)
                std::fprintf(stderr, "glyph U+%04X leaves its box\n", static_cast<unsigned>(c));
        }

        // Capitals and digits sit on the baseline and reach the cap height.
        for (const char32_t c : std::u32string_view(U"EHILT0123456789"))
        {
            float low = 1e30f, high = -1e30f;
            for (const auto &stroke : find_glyph(c).strokes)
                for (const math::vec2f p : stroke)
                {
                    low = std::min(low, p.y());
                    high = std::max(high, p.y());
                }
            CHECK(near(low, 0.0f, 0.01f) && near(high, 1.0f, 0.01f));
        }
        // Lowercase letters without ascenders reach the x height; those with
        // descenders go below the baseline, to the descent.
        for (const char32_t c : std::u32string_view(U"acemnorsuvwxz"))
        {
            float high = -1e30f;
            for (const auto &stroke : find_glyph(c).strokes)
                for (const math::vec2f p : stroke)
                    high = std::max(high, p.y());
            CHECK(near(high, font_x_height, 0.01f));
        }
        for (const char32_t c : std::u32string_view(U"gjpqy"))
        {
            float low = 1e30f;
            for (const auto &stroke : find_glyph(c).strokes)
                for (const math::vec2f p : stroke)
                    low = std::min(low, p.y());
            CHECK(near(low, -font_descent, 0.01f));
        }

        // Digits share one advance.
        for (char32_t c = U'0'; c <= U'9'; ++c)
            CHECK(find_glyph(c).advance == find_glyph(U'0').advance);

        // Anything else is the box, one glyph for all.
        CHECK(!has_glyph(U'一') && !has_glyph(U'€') && !has_glyph(0) && !has_glyph(U'\n'));
        CHECK(!unknown.strokes.empty() && &find_glyph(U'€') == &unknown && &find_glyph(U'\n') == &unknown &&
              &find_glyph(0x1F600) == &unknown);
        CHECK(&find_glyph(U'A') == &find_glyph(U'A'));
    }

    void widths()
    {
        const std::string_view text = "R12 4.7k\xCE\xA9";
        const float one = text_width(text, 1.0f);
        CHECK(one > 0.0f);
        CHECK(near(text_width(text, 2.0f), 2.0f * one) && near(text_width(text, 0.35f), 0.35f * one));
        CHECK(near(text_width("AB", 1.0f), text_width("A", 1.0f) + text_width("B", 1.0f)));
        CHECK(near(text_width("A", 1.0f), find_glyph(U'A').advance));
        CHECK(text_width("", 1.0f) == 0.0f && text_width("abc", 0.0f) == 0.0f);
        CHECK(text_width("W", 1.0f) > text_width("i", 1.0f));

        // The drawing scales the same way, about the anchor.
        const auto small = draw(text, {0.0f, 0.0f}, 1.0f, horizontal_align::left, vertical_align::bottom, 0);
        const auto large = draw(text, {0.0f, 0.0f}, 3.0f, horizontal_align::left, vertical_align::bottom, 0);
        CHECK(!small.empty() && small.size() == large.size());
        bool scaled = small.size() == large.size();
        for (std::size_t i = 0; scaled && i < small.size(); ++i)
            scaled = near(large[i].a.x(), 3.0f * small[i].a.x(), 1e-3f) &&
                     near(large[i].b.y(), 3.0f * small[i].b.y(), 1e-3f);
        CHECK(scaled);

        CHECK(draw("", {1.0f, 1.0f}, 1.0f, horizontal_align::left, vertical_align::bottom, 0).empty());
        CHECK(draw("   ", {1.0f, 1.0f}, 1.0f, horizontal_align::left, vertical_align::bottom, 0).empty());
        CHECK(draw("A", {1.0f, 1.0f}, 0.0f, horizontal_align::left, vertical_align::bottom, 0).empty());
    }

    void alignment()
    {
        // "HHH": strokes from the baseline to the cap height, the same
        // space either side.
        const std::string_view text = "HHH";
        const math::vec2f anchor{10.0f, 20.0f};
        const float h = 2.0f;
        const float width = text_width(text, h);
        const float side = (find_glyph(U'H').advance - 0.6f) * 0.5f * h; // H is 0.6 wide

        {
            const extent e = extent_of(draw(text, anchor, h, horizontal_align::left, vertical_align::bottom, 0));
            CHECK(near(e.min_x, anchor.x() + side, 1e-3f) && near(e.max_x, anchor.x() + width - side, 1e-3f));
            CHECK(near(e.min_y, anchor.y()) && near(e.max_y, anchor.y() + h));
        }
        {
            const extent e = extent_of(draw(text, anchor, h, horizontal_align::center, vertical_align::middle, 0));
            CHECK(near(0.5f * (e.min_x + e.max_x), anchor.x(), 1e-3f));
            CHECK(near(0.5f * (e.min_y + e.max_y), anchor.y(), 1e-3f));
        }
        {
            const extent e = extent_of(draw(text, anchor, h, horizontal_align::right, vertical_align::top, 0));
            CHECK(near(e.max_x, anchor.x() - side, 1e-3f) && near(e.max_y, anchor.y()) &&
                  near(e.min_y, anchor.y() - h));
        }

        // Once: reading upwards from the anchor, the letters to its left.
        {
            const extent e = extent_of(draw(text, anchor, h, horizontal_align::left, vertical_align::bottom, 1));
            CHECK(near(e.min_y, anchor.y() + side, 1e-3f) && near(e.max_y, anchor.y() + width - side, 1e-3f));
            CHECK(near(e.max_x, anchor.x()) && near(e.min_x, anchor.x() - h));
        }
        // Twice: reads as unturned, but ends at the anchor and hangs from it.
        {
            const extent e = extent_of(draw(text, anchor, h, horizontal_align::left, vertical_align::bottom, 2));
            CHECK(near(e.max_x, anchor.x() - side, 1e-3f) && near(e.min_x, anchor.x() - width + side, 1e-3f));
            CHECK(near(e.max_y, anchor.y()) && near(e.min_y, anchor.y() - h));
        }
        // Three times: reads upwards, ending at the anchor, letters to its
        // right.
        {
            const extent e = extent_of(draw(text, anchor, h, horizontal_align::left, vertical_align::bottom, 3));
            CHECK(near(e.max_y, anchor.y() - side, 1e-3f) && near(e.min_y, anchor.y() - width + side, 1e-3f));
            CHECK(near(e.min_x, anchor.x()) && near(e.max_x, anchor.x() + h));
        }
        // Turns are taken modulo 4, negative ones too.
        CHECK(same(draw(text, anchor, h, horizontal_align::left, vertical_align::bottom, 5),
                   draw(text, anchor, h, horizontal_align::left, vertical_align::bottom, 1)));
        CHECK(same(draw(text, anchor, h, horizontal_align::left, vertical_align::bottom, -1),
                   draw(text, anchor, h, horizontal_align::left, vertical_align::bottom, 3)));

        // Never upside down or mirrored: twice is unturned with the anchor
        // on the other side, three times is once likewise.
        const std::string_view lopsided = "L7?Fg";
        for (const auto [hz, hz_flip] : {std::pair{horizontal_align::left, horizontal_align::right},
                                         std::pair{horizontal_align::center, horizontal_align::center},
                                         std::pair{horizontal_align::right, horizontal_align::left}})
            for (const auto [vt, vt_flip] : {std::pair{vertical_align::bottom, vertical_align::top},
                                             std::pair{vertical_align::middle, vertical_align::middle},
                                             std::pair{vertical_align::top, vertical_align::bottom}})
            {
                CHECK(same(draw(lopsided, anchor, h, hz, vt, 2), draw(lopsided, anchor, h, hz_flip, vt_flip, 0)));
                CHECK(same(draw(lopsided, anchor, h, hz, vt, 3), draw(lopsided, anchor, h, hz_flip, vt_flip, 1)));
            }
        // And once is the unturned drawing turned a quarter counter-
        // clockwise about the anchor: not a mirror image.
        {
            const auto flat = draw(lopsided, anchor, h, horizontal_align::left, vertical_align::bottom, 0);
            const auto up = draw(lopsided, anchor, h, horizontal_align::left, vertical_align::bottom, 1);
            bool turned = flat.size() == up.size();
            const auto turn = [&](math::vec2f p) {
                const math::vec2f d = p - anchor;
                return anchor + math::vec2f{-d.y(), d.x()};
            };
            for (std::size_t i = 0; turned && i < flat.size(); ++i)
            {
                const math::vec2f a = turn(flat[i].a);
                const math::vec2f b = turn(flat[i].b);
                turned = near(a.x(), up[i].a.x(), 1e-3f) && near(a.y(), up[i].a.y(), 1e-3f) &&
                         near(b.x(), up[i].b.x(), 1e-3f) && near(b.y(), up[i].b.y(), 1e-3f);
            }
            CHECK(turned);
        }

        // text_bounds() holds every stroke, in every turn and alignment, and
        // layout_text() agrees with it.
        for (int turns = 0; turns < 4; ++turns)
            for (const horizontal_align hz : {horizontal_align::left, horizontal_align::center, horizontal_align::right})
                for (const vertical_align vt : {vertical_align::bottom, vertical_align::middle, vertical_align::top})
                {
                    const std::string_view sample = "Wgjy|[}";
                    math::vec2f lo, hi;
                    text_bounds(sample, anchor, h, hz, vt, turns, lo, hi);
                    const extent e = extent_of(draw(sample, anchor, h, hz, vt, turns));
                    // Brackets reach above the cap height, past the box.
                    const float slack = (font_ascent - 1.0f) * h + 1e-3f;
                    CHECK(e.min_x >= lo.x() - slack && e.max_x <= hi.x() + slack && e.min_y >= lo.y() - slack &&
                          e.max_y <= hi.y() + slack);
                    const float area = (hi.x() - lo.x()) * (hi.y() - lo.y());
                    CHECK(near(area, text_width(sample, h) * (1.0f + font_descent) * h, 1e-2f));
                }
        const text_frame frame = layout_text(4.0f, {1.0f, 1.0f}, 1.0f, horizontal_align::center,
                                             vertical_align::middle, 1);
        CHECK(near(frame.origin.x(), 1.5f) && near(frame.origin.y(), -1.0f));
        CHECK(frame.along == math::vec2f{0.0f, 1.0f} && frame.up == math::vec2f{-1.0f, 0.0f});
    }

    void utf8()
    {
        CHECK(decode_utf8("A\xCE\xA9\xC2\xB5\xCE\xBC\xC2\xB0\xC2\xB1") == U"AΩµμ°±");
        CHECK(decode_utf8("\xF0\x9F\x98\x80") == U"\U0001F600");
        CHECK(decode_utf8("") == U"");

        // Ω and the two µ are glyphs, not boxes; the two µ are the same.
        CHECK(near(text_width("\xCE\xA9", 1.0f), find_glyph(U'Ω').advance));
        const auto micro = draw("\xC2\xB5", {0.0f, 0.0f}, 1.0f, horizontal_align::left, vertical_align::bottom, 0);
        const auto mu = draw("\xCE\xBC", {0.0f, 0.0f}, 1.0f, horizontal_align::left, vertical_align::bottom, 0);
        const auto box = draw("\xE2\x82\xAC", {0.0f, 0.0f}, 1.0f, horizontal_align::left, vertical_align::bottom, 0);
        CHECK(!micro.empty() && same(micro, mu) && !same(micro, box));
        const auto omega = draw("\xCE\xA9", {0.0f, 0.0f}, 1.0f, horizontal_align::left, vertical_align::bottom, 0);
        CHECK(!omega.empty() && !same(omega, box));

        // Bad bytes: each one is a replacement character, drawn as a box,
        // and whatever follows is read again.
        CHECK(decode_utf8("\xFF") == U"�");
        CHECK(decode_utf8("\xC3(") == U"�(");
        CHECK(decode_utf8("\xE2\x82") == U"��");
        CHECK(decode_utf8("\xC0\xAF") == U"��");          // overlong
        CHECK(decode_utf8("\xED\xA0\x80") == U"���"); // a surrogate
        CHECK(decode_utf8("\xF4\x90\x80\x80") == U"����"); // past U+10FFFF
        CHECK(decode_utf8("a\x80" "b") == U"a�b");
        for (const std::string_view bad : {std::string_view("\xFF\xFE\xFD"), std::string_view("\xE2\x82"),
                                           std::string_view("\xF0\x9F\x98"), std::string_view("abc\xC3"),
                                           std::string_view("\x80\x80\x80\x80\x80")})
        {
            const auto out = draw(bad, {1.0f, 2.0f}, 1.0f, horizontal_align::center, vertical_align::middle, 1);
            bool finite = !out.empty();
            for (const segment &s : out)
                finite = finite && std::isfinite(s.a.x()) && std::isfinite(s.a.y()) && std::isfinite(s.b.x()) &&
                         std::isfinite(s.b.y());
            CHECK(finite);
            CHECK(text_width(bad, 1.0f) > 0.0f);
        }
        // Control characters are boxes too.
        CHECK(near(text_width("\t", 1.0f), find_glyph(U'一').advance));
    }

    void segments_out()
    {
        // Strokes come out as the glyph's polylines: a dot as a segment of
        // no length, every segment of the stroke width, colour and node -1.
        const math::vec4f color{0.25f, 0.5f, 0.75f, 0.5f};
        std::vector<segment> out;
        out.push_back({});
        append_text(out, ".", {3.0f, 4.0f}, 1.0f, horizontal_align::left, vertical_align::bottom, 0, 0.2f, color);
        CHECK(out.size() == 2 && out[1].a == out[1].b && out[1].width == 0.2f && out[1].color == color &&
              out[1].node == -1);
        CHECK(near(out[1].a.y(), 4.0f));
        const std::size_t before = out.size();
        append_text(out, "H", {0.0f, 0.0f}, 1.0f, horizontal_align::left, vertical_align::bottom, 0, 0.2f, color);
        CHECK(out.size() == before + 3);
    }

} // namespace

int main()
{
    glyphs();
    widths();
    alignment();
    utf8();
    segments_out();
    return volt::tests::result("schematic stroke font");
}
