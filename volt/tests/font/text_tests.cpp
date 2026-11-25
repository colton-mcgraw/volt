#include "common.hpp"

#include <font/text.hpp>
#include <font/utf8.hpp>

#include <string>

using namespace volt::font;
using volt::math::vec2f;
using volt::tests::font::arial;
using volt::tests::font::near;
using volt::tests::font::saira;

namespace
{
    // ---- UTF-8 -----------------------------------------------------------

    std::u32string decode_all(std::string_view text)
    {
        std::u32string out;
        for (std::size_t offset = 0; offset < text.size();)
        {
            const decoded d = decode_utf8(text, offset);
            out.push_back(d.code_point);
            offset += d.length;
        }
        return out;
    }

    void utf8()
    {
        constexpr char32_t r = replacement_character;
        CHECK(decode_all("A") == U"A");
        CHECK(decode_all("\xC3\xA9") == U"é");
        CHECK(decode_all("\xE2\x82\xAC") == U"€");
        CHECK(decode_all("\xF0\x9F\x98\x80") == U"\U0001F600");
        CHECK(decode_all("\xEF\xBF\xBF") == U"￿"); // a noncharacter, but well-formed
        CHECK(decode_all("\xF4\x8F\xBF\xBF") == U"\U0010FFFF");

        // Ill-formed: one replacement per maximal subpart.
        CHECK(decode_all("\xC0\xAF") == std::u32string{r, r});          // overlong '/'
        CHECK(decode_all("\xE0\x80\xAF") == std::u32string{r, r, r});   // overlong
        CHECK(decode_all("\xED\xA0\x80") == std::u32string{r, r, r});   // surrogate D800
        CHECK(decode_all("\xF4\x90\x80\x80") == std::u32string{r, r, r, r}); // above U+10FFFF
        CHECK(decode_all("\xF5\xFF") == std::u32string{r, r});
        CHECK(decode_all("\xE2\x82") == std::u32string{r});              // truncated at the end
        CHECK(decode_all("\xE2\x82" "A") == std::u32string{r, U'A'});    // truncated before ASCII
        CHECK(decode_utf8("\xE2\x82", 0).length == 2);
        // The example of Table 3-8 in the Unicode standard.
        CHECK(decode_all("\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64") ==
              std::u32string{U'a', r, r, r, U'b', r, U'c', r, r, U'd'});
        CHECK(decode_utf8("abc", 3).length == 0);
        CHECK(decode_utf8("", 0).length == 0);

        // Encoding round trips; what cannot be encoded becomes U+FFFD.
        for (const char32_t c : {U'\0', U'A', U'\u007F', U'\u0080', U'߿', U'ࠀ', U'�', U'￿',
                                 U'\U00010000', U'\U0010FFFF'})
        {
            std::string encoded;
            append_utf8(encoded, c);
            const decoded d = decode_utf8(encoded, 0);
            CHECK(d.code_point == c && d.length == encoded.size());
        }
        std::string bad;
        append_utf8(bad, 0xD800);
        append_utf8(bad, 0x110000);
        CHECK(bad == "\xEF\xBF\xBD\xEF\xBF\xBD");

        // Stepping back finds exactly the boundaries stepping forward does,
        // valid or not.
        const std::string mixed = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\x80\x80\xE2\x82z\xF1\x80\x80\x80\x80\xC2";
        std::vector<std::size_t> forward{0};
        for (std::size_t offset = 0; offset < mixed.size();)
            forward.push_back(offset = next_boundary(mixed, offset));
        std::vector<std::size_t> backward{mixed.size()};
        for (std::size_t offset = mixed.size(); offset > 0;)
            backward.insert(backward.begin(), offset = previous_boundary(mixed, offset));
        CHECK(forward == backward);
        CHECK(count_code_points(mixed) == forward.size() - 1);
        CHECK(next_boundary(mixed, 1000) == mixed.size());
        CHECK(previous_boundary(mixed, 1000) == previous_boundary(mixed, mixed.size()));
        CHECK(previous_boundary(mixed, 0) == 0);
    }

    // ---- shaping -----------------------------------------------------------

    void chains()
    {
        const font_chain fonts({saira(), arial()});
        CHECK(fonts.primary().id() == saira().id());
        CHECK(fonts.resolve(U'A').font == 0 && fonts.resolve(U'A').glyph == 1);
        // Hebrew alef: not in Saira, in Arial.
        CHECK(fonts.resolve(U'א').font == 1 && fonts.resolve(U'א').glyph == arial().glyph(U'א'));
        // In neither: the primary font's .notdef.
        CHECK(fonts.resolve(U'一').font == 0 && fonts.resolve(U'一').glyph == 0);

        const font_chain none;
        CHECK(none.empty() && !none.primary().valid());
        CHECK(none.resolve(U'A').glyph == 0);
        const shaped_run nothing = shape(none, "abc", 16.0f);
        CHECK(nothing.glyphs.size() == 3 && nothing.width == 0.0f);
    }

    void shaping()
    {
        // At 2048, Arial's em, one unit is one unit.
        const font_chain fonts({arial()});
        const shaped_run av = shape(fonts, "AV", 2048.0f);
        CHECK(av.glyphs.size() == 2);
        CHECK(near(av.glyphs[0].advance, 1366.0f - 152.0f, 1e-3f));
        CHECK(near(av.glyphs[1].x, 1214.0f, 1e-3f));
        CHECK(near(av.width, 2580.0f, 1e-3f));
        CHECK(near(measure(fonts, "To", 2048.0f), 1251.0f - 227.0f + 1139.0f, 1e-3f));
        CHECK(near(measure(fonts, "To", 16.0f), (1251.0f - 227.0f + 1139.0f) * 16.0f / 2048.0f, 1e-4f));
        CHECK(near(measure(font_chain({saira()}), "AV", 1000.0f), 683.0f - 60.0f + 663.0f, 1e-3f));

        // Clusters are byte offsets.
        const shaped_run accented = shape(fonts, "aé€!", 2048.0f);
        CHECK(accented.glyphs.size() == 4 && accented.length == 7);
        CHECK(accented.glyphs[1].cluster == 1 && accented.glyphs[2].cluster == 3 && accented.glyphs[3].cluster == 6);
        CHECK(accented.glyphs[1].glyph == 112);

        // A tab is four spaces; invisible characters take no room.
        CHECK(near(measure(fonts, "\t", 2048.0f), 4.0f * 569.0f, 1e-3f));
        CHECK(near(measure(fonts, "a​b", 2048.0f), measure(fonts, "ab", 2048.0f), 1e-3f));
        CHECK(near(measure(fonts, "a\nb", 2048.0f), measure(fonts, "ab", 2048.0f), 1e-3f));
        // A kerning pair split by an invisible character is not kerned.
        CHECK(near(measure(fonts, "A​V", 2048.0f), 2.0f * 1366.0f, 1e-3f));

        // Kerning does not reach across fonts: Saira's A then Arial's alef.
        const font_chain mixed({saira(), arial()});
        const shaped_run run = shape(mixed, "Aא", 1000.0f);
        CHECK(run.glyphs[1].font == 1);
        CHECK(near(run.width, 683.0f + 1153.0f * 1000.0f / 2048.0f, 1e-2f));

        // Garbage in, replacement characters out, no surprises.
        const shaped_run broken = shape(fonts, "\xFF\xFE" "a", 16.0f);
        CHECK(broken.glyphs.size() == 3 && broken.glyphs[2].cluster == 2);
    }

    void carets()
    {
        const font_chain fonts({arial()});
        const shaped_run run = shape(fonts, "Toé", 2048.0f); // T o é: offsets 0 1 2, length 4
        const std::vector<caret_stop> stops = caret_stops(run);
        CHECK(stops.size() == 4);
        CHECK(stops[0].offset == 0 && stops[1].offset == 1 && stops[2].offset == 2 && stops[3].offset == 4);
        CHECK(near(stops[1].x, 1251.0f - 227.0f, 1e-3f));
        CHECK(near(stops[3].x, run.width, 1e-3f));
        for (std::size_t i = 1; i < stops.size(); ++i)
            CHECK(stops[i].x > stops[i - 1].x);

        CHECK(near(caret_x(run, 0), 0.0f, 1e-6f));
        CHECK(near(caret_x(run, 2), stops[2].x, 1e-3f));
        CHECK(near(caret_x(run, 3), stops[2].x, 1e-3f)); // inside é: its start
        CHECK(near(caret_x(run, 99), run.width, 1e-3f));

        CHECK(hit_test(run, -100.0f) == 0);
        CHECK(hit_test(run, stops[1].x - 1.0f) == 1); // just before the o: past the middle of T
        CHECK(hit_test(run, stops[1].x + 1.0f) == 1);
        CHECK(hit_test(run, (stops[2].x + stops[3].x) * 0.5f + 1.0f) == 4);
        CHECK(hit_test(run, 1e9f) == 4);
        for (const caret_stop &stop : stops)
            CHECK(hit_test(run, stop.x) == stop.offset);

        const shaped_run empty = shape(fonts, "", 16.0f);
        CHECK(caret_stops(empty).size() == 1 && hit_test(empty, 5.0f) == 0 && caret_x(empty, 0) == 0.0f);
    }

    // ---- layout -----------------------------------------------------------

    constexpr float a_width = 1139.0f;   // Arial 'a' at 2048
    constexpr float ascent = 1854.0f;    // Arial hhea
    constexpr float line_height = 2355.0f;
    constexpr float ellipsis_width = 2048.0f;

    layout_options at_em(float max_width)
    {
        layout_options options;
        options.size = 2048.0f;
        options.max_width = max_width;
        return options;
    }

    void wrapping()
    {
        const font_chain fonts({arial()});

        const text_layout three = layout(fonts, "aaa aaa aaa", at_em(5000.0f));
        CHECK(three.lines.size() == 3);
        if (three.lines.size() == 3)
        {
            CHECK(three.lines[0].begin == 0 && three.lines[0].end == 4);
            CHECK(three.lines[1].begin == 4 && three.lines[1].end == 8);
            CHECK(three.lines[2].begin == 8 && three.lines[2].end == 11);
            CHECK(near(three.lines[0].width, 3.0f * a_width, 1e-2f)); // the space hangs
            CHECK(near(three.lines[0].baseline, ascent, 1e-2f));
            CHECK(near(three.lines[2].baseline, ascent + 2.0f * line_height, 1e-2f));
            CHECK(three.lines[0].glyph_count == 4 && three.lines[2].glyph_count == 3);
            CHECK(!three.lines[0].hard_break && !three.lines[0].truncated);
            CHECK(near(three.glyphs[4].position[0], 0.0f, 1e-3f)); // second line starts at the left
            CHECK(near(three.glyphs[5].position[0], a_width, 1e-2f));
            CHECK(near(three.glyphs[5].position[1], ascent + line_height, 1e-2f));
        }
        CHECK(near(three.extent[0], 5000.0f, 1e-3f) && near(three.extent[1], 3.0f * line_height, 1e-2f));

        // Exactly fitting does not wrap; one unit less does.
        CHECK(layout(fonts, "aaa aaa", at_em(7.0f * a_width - 1139.0f + 569.0f + 1e-2f)).lines.size() == 1);
        CHECK(layout(fonts, "aaa aaa", at_em(7.0f * a_width - 1139.0f + 569.0f - 1.0f)).lines.size() == 2);

        // Without a width, only line breaks in the text break lines.
        const text_layout breaks = layout(fonts, "ab\ncd\r\nef\n", at_em(0.0f));
        CHECK(breaks.lines.size() == 4);
        if (breaks.lines.size() == 4)
        {
            CHECK(breaks.lines[0].begin == 0 && breaks.lines[0].end == 2 && breaks.lines[0].hard_break);
            CHECK(breaks.lines[1].begin == 3 && breaks.lines[1].end == 6); // the CR stays, the LF ends it
            CHECK(breaks.lines[2].begin == 7 && breaks.lines[2].end == 9);
            CHECK(breaks.lines[3].begin == 10 && breaks.lines[3].end == 10 && !breaks.lines[3].hard_break);
            CHECK(breaks.lines[0].glyph_count == 2); // the LF is no glyph
        }

        const text_layout empty = layout(fonts, "", at_em(1000.0f));
        CHECK(empty.lines.size() == 1 && empty.glyphs.empty() && empty.lines[0].end == 0);

        // A word wider than the line breaks between letters.
        const text_layout word = layout(fonts, "aaaaaaaa", at_em(2.5f * a_width));
        CHECK(word.lines.size() == 4);
        for (const text_line &line : word.lines)
            CHECK(line.glyph_count == 2);
        // Even one letter wider than the line gets a line of its own.
        CHECK(layout(fonts, "aaa", at_em(10.0f)).lines.size() == 3);

        // Hyphens are break opportunities too.
        const text_layout hyphen = layout(fonts, "aa-aa", at_em(3.5f * a_width));
        CHECK(hyphen.lines.size() == 2 && hyphen.lines[0].end == 3);
    }

    void alignment()
    {
        const font_chain fonts({arial()});
        layout_options options = at_em(10000.0f);
        options.align = text_align::center;
        const text_layout centered = layout(fonts, "aa\naaaa", options);
        CHECK(near(centered.lines[0].x, (10000.0f - 2.0f * a_width) * 0.5f, 1e-2f));
        CHECK(near(centered.glyphs[0].position[0], centered.lines[0].x, 1e-3f));
        options.align = text_align::right;
        const text_layout right = layout(fonts, "aa\naaaa", options);
        CHECK(near(right.lines[1].x, 10000.0f - 4.0f * a_width, 1e-2f));
        CHECK(near(right.glyphs.back().position[0] + right.glyphs.back().advance, 10000.0f, 1e-2f));

        // With no width, within the widest line.
        options.max_width = 0.0f;
        const text_layout free = layout(fonts, "aa\naaaa", options);
        CHECK(near(free.extent[0], 4.0f * a_width, 1e-2f));
        CHECK(near(free.lines[0].x, 2.0f * a_width, 1e-2f));

        options.line_spacing = 1.5f;
        CHECK(near(layout(fonts, "a\nb", options).lines[1].baseline, ascent + 1.5f * line_height, 1e-2f));
    }

    void truncation()
    {
        const font_chain fonts({arial()});

        // Two lines allowed of three: the second is cut to fit "aa…".
        layout_options options = at_em(5000.0f);
        options.max_lines = 2;
        const text_layout cut = layout(fonts, "aaa aaa aaa", options);
        CHECK(cut.lines.size() == 2);
        if (cut.lines.size() == 2)
        {
            const text_line &line = cut.lines[1];
            CHECK(line.truncated && line.glyph_count == 3);
            CHECK(cut.glyphs.back().glyph == arial().glyph(U'…'));
            CHECK(cut.glyphs.back().cluster == 6);
            CHECK(near(line.width, 2.0f * a_width + ellipsis_width, 1e-2f));
            CHECK(line.width <= 5000.0f);
        }

        // One line, no wrapping: cut where it runs out.
        options = at_em(5000.0f);
        options.wrap = false;
        const text_layout single = layout(fonts, "aaaaaaaa", options);
        CHECK(single.lines.size() == 1 && single.lines[0].truncated && single.lines[0].glyph_count == 3);
        // Fitting text is left alone.
        CHECK(!layout(fonts, "aaaa", options).lines[0].truncated);

        // Without the ellipsis, the cut is clean.
        options.ellipsis = false;
        const text_layout clipped = layout(fonts, "aaa aaa aaa", [&] {
            layout_options o = at_em(5000.0f);
            o.max_lines = 1;
            o.ellipsis = false;
            return o;
        }());
        CHECK(clipped.lines.size() == 1 && clipped.lines[0].truncated && clipped.lines[0].glyph_count == 4);
    }

    void layout_carets()
    {
        const font_chain fonts({arial()});
        const text_layout text = layout(fonts, "aaa aaa aaa", at_em(5000.0f));

        // At a wrap the caret goes to the next line.
        CHECK(caret_at(text, 3).line == 0 && near(caret_at(text, 3).top[0], 3.0f * a_width, 1e-2f));
        CHECK(caret_at(text, 4).line == 1 && near(caret_at(text, 4).top[0], 0.0f, 1e-3f));
        CHECK(caret_at(text, 11).line == 2 && near(caret_at(text, 11).top[0], 3.0f * a_width, 1e-2f));
        CHECK(near(caret_at(text, 0).top[1], 0.0f, 1e-3f));
        CHECK(near(caret_at(text, 5).top[1], line_height, 1e-2f));
        CHECK(near(caret_at(text, 5).height, 1854.0f + 434.0f, 1e-2f));
        CHECK(caret_at(text, 1000).line == 2);

        // Clicks.
        CHECK(hit_test(text, vec2f{-10.0f, 10.0f}) == 0);
        CHECK(hit_test(text, vec2f{1e6f, 10.0f}) == 3); // past the end of a wrapped line: before its space
        CHECK(hit_test(text, vec2f{-10.0f, line_height + 10.0f}) == 4);
        CHECK(hit_test(text, vec2f{1.2f * a_width, line_height + 10.0f}) == 5);
        CHECK(hit_test(text, vec2f{1e6f, 1e6f}) == 11);
        CHECK(hit_test(text, vec2f{10.0f, -1e6f}) == 0);

        // Every caret position clicks back to itself.
        for (std::size_t offset = 0; offset <= 11; ++offset)
        {
            const caret c = caret_at(text, offset);
            const std::size_t back = hit_test(text, c.top + vec2f{0.0f, c.height * 0.5f});
            CHECK(back == offset);
        }

        // Hard breaks: the end of the line is the line break's offset.
        const text_layout breaks = layout(fonts, "ab\ncd", at_em(0.0f));
        CHECK(caret_at(breaks, 2).line == 0 && caret_at(breaks, 3).line == 1);
        CHECK(hit_test(breaks, vec2f{1e6f, 10.0f}) == 2);
        CHECK(hit_test(breaks, vec2f{1e6f, line_height + 10.0f}) == 5);
    }

    void geometry()
    {
        const font_chain fonts({arial()});
        const text_layout text = layout(fonts, "A", at_em(0.0f));
        const outline shape = layout_outline(fonts, text);
        const box bounds = shape.control_bounds();
        // A's box is (-3, 0) to (1369, 1466), now y down from the baseline.
        CHECK(near(bounds.min[0], -3.0f, 1e-2f) && near(bounds.max[0], 1369.0f, 1e-2f));
        CHECK(near(bounds.min[1], ascent - 1466.0f, 1e-2f) && near(bounds.max[1], ascent, 1e-2f));

        const text_layout spaces = layout(fonts, "   ", at_em(0.0f));
        CHECK(layout_outline(fonts, spaces).empty());
    }
} // namespace

int main()
{
    utf8();
    chains();
    shaping();
    carets();
    wrapping();
    alignment();
    truncation();
    layout_carets();
    geometry();
    return volt::tests::result("font text");
}
