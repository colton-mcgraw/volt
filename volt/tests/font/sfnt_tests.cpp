#include "common.hpp"

#include <cstring>


using namespace volt::font;
using volt::tests::font::arial;
using volt::tests::font::saira;

namespace
{
    bool near_scale(const font &f)
    {
        return volt::tests::font::near(f.scale(2048.0f), 1.0f, 1e-6f) &&
               volt::tests::font::near(f.scale(16.0f), 16.0f / 2048.0f, 1e-9f);
    }

    box make_box(float x_min, float y_min, float x_max, float y_max)
    {
        box result;
        result.add({x_min, y_min});
        result.add({x_max, y_max});
        return result;
    }

    void arial_tables()
    {
        const font f = arial();
        CHECK(f.valid());
        CHECK(f.id() != 0);
        CHECK(f.format() == outline_format::truetype);
        CHECK(f.family() == "Arial");
        CHECK(f.style() == "Regular");
        CHECK(f.units_per_em() == 2048);
        CHECK(f.glyph_count() == 4551);
        CHECK(near_scale(f));

        const font_metrics &m = f.metrics();
        CHECK(m.hhea == line_metrics{1854, -434, 67});
        CHECK(m.typo == line_metrics{1491, -431, 307});
        CHECK(m.win == line_metrics{1854, -434, 0});
        // USE_TYPO_METRICS is not set (fsSelection 0x40), so hhea it is.
        CHECK(m.line == m.hhea);
        CHECK(m.line.height() == 1854 + 434 + 67);
        CHECK(m.x_height == 1062);
        CHECK(m.cap_height == 1467);
        CHECK(m.underline.position == -217 && m.underline.thickness == 150);
        CHECK(m.strikeout.position == 530 && m.strikeout.thickness == 102);
        CHECK(m.bounds == make_box(-1361, -665, 4096, 2129));
    }

    void saira_tables()
    {
        const font f = saira();
        CHECK(f.valid());
        CHECK(f.family() == "Saira Stencil"); // typographic family (name 16)
        CHECK(f.style() == "Thin");
        CHECK(f.units_per_em() == 1000);
        CHECK(f.glyph_count() == 887);
        CHECK(f.id() != arial().id());

        const font_metrics &m = f.metrics();
        // fsSelection 0xC0 has USE_TYPO_METRICS.
        CHECK(m.line == line_metrics{1135, -439, 0});
        CHECK(m.win == line_metrics{1165, -439, 0});
        CHECK(m.x_height == 510);
        CHECK(m.cap_height == 688);
        CHECK(m.underline.position == -160 && m.underline.thickness == 35);
        CHECK(m.strikeout.position == 306 && m.strikeout.thickness == 35);
    }

    struct expected_glyph
    {
        char32_t code_point;
        glyph_id glyph;
        int advance;
        int left_side_bearing;
        bool blank;
        float x_min, y_min, x_max, y_max;
    };

    void glyphs(const font &f, std::initializer_list<expected_glyph> expected)
    {
        for (const expected_glyph &e : expected)
        {
            const glyph_id glyph = f.glyph(e.code_point);
            CHECK(glyph == e.glyph);
            const glyph_metrics m = f.metrics(glyph);
            CHECK(m.advance == e.advance);
            CHECK(f.advance(glyph) == e.advance);
            CHECK(m.left_side_bearing == e.left_side_bearing);
            if (e.blank)
                CHECK(m.bounds.empty());
            else
                CHECK(m.bounds == make_box(e.x_min, e.y_min, e.x_max, e.y_max));
        }
    }

    void cmap_and_metrics()
    {
        glyphs(arial(), {
                            {U'A', 36, 1366, -3, false, -3, 0, 1369, 1466},
                            {U' ', 3, 569, 0, true, 0, 0, 0, 0},
                            {U'g', 74, 1139, 66, false, 66, -431, 1002, 1086},
                            {U'.', 17, 569, 186, false, 186, 0, 391, 205},
                            {U'é', 112, 1139, 75, false, 75, -24, 1054, 1474},
                            {U'Å', 99, 1366, -3, false, -3, 0, 1369, 1780},
                            {U'—', 178, 2048, 0, false, 0, 458, 2048, 603},
                            {U'€', 188, 1139, -28, false, -28, -25, 1107, 1491},
                        });
        glyphs(saira(), {
                            {U'A', 1, 683, 40, false, 40, 0, 643, 688},
                            {U' ', 631, 224, 0, true, 0, 0, 0, 0},
                            {U'g', 340, 596, 75, false, 75, -206, 496, 518},
                            {U'é', 319, 562, 75, false, 75, -8, 487, 744},
                            {U'—', 668, 871, 65, false, 65, 287, 806, 321},
                            {U'€', 721, 625, 41, false, 41, -8, 560, 696},
                        });

        // Characters neither font has, and values that are not characters.
        CHECK(arial().glyph(U'\U0001F600') == 0);
        CHECK(saira().glyph(U'一') == 0);
        CHECK(arial().glyph(0x110000) == 0);
        CHECK(arial().glyph(0xFFFFFFFF) == 0);
        CHECK(!arial().has_glyph(U'￿'));

        // Out of range glyph ids are zeros, not reads past the end.
        const glyph_metrics none = arial().metrics(glyph_id{60000});
        CHECK(none.advance == 0 && none.bounds.empty());
        CHECK(!arial().glyph_outline(glyph_id{60000}));
    }

    // Glyphs past numberOfHMetrics share the last advance.
    void trailing_metrics()
    {
        // Arial: 4549 long metrics for 4551 glyphs; Saira: 886 for 887.
        const font f = arial();
        CHECK(f.advance(4550) == f.advance(4548));
        CHECK(saira().advance(886) == saira().advance(885));
    }

    // Outlines: the control box of every glyph equals the box its 'glyf'
    // header stores, for simple and composite glyphs alike.
    void outline_boxes(const font &f, std::size_t allowed_mismatches)
    {
        std::size_t mismatches = 0;
        std::size_t outlines = 0;
        for (std::uint32_t id = 0; id < f.glyph_count(); ++id)
        {
            const auto glyph = static_cast<glyph_id>(id);
            const auto shape = f.glyph_outline(glyph);
            CHECK(shape.has_value());
            if (!shape)
                continue;
            const box header = f.metrics(glyph).bounds;
            if (shape->empty())
            {
                // Blank glyphs have no box; a few fonts keep a header anyway.
                continue;
            }
            ++outlines;
            // Within half a unit: a composite scaled by a fraction (Arial's
            // glyph 893 is mirrored by -0.99988) has a fractional box, which
            // the font's header stores rounded.
            const box control = shape->control_bounds();
            const bool same = volt::tests::font::near(control.min[0], header.min[0], 0.5f) &&
                              volt::tests::font::near(control.min[1], header.min[1], 0.5f) &&
                              volt::tests::font::near(control.max[0], header.max[0], 0.5f) &&
                              volt::tests::font::near(control.max[1], header.max[1], 0.5f);
            if (!same)
            {
                if (++mismatches <= 3)
                    std::fprintf(stderr, "  glyph %u: outline box (%g %g %g %g) header (%g %g %g %g)\n", id,
                                 static_cast<double>(shape->control_bounds().min[0]),
                                 static_cast<double>(shape->control_bounds().min[1]),
                                 static_cast<double>(shape->control_bounds().max[0]),
                                 static_cast<double>(shape->control_bounds().max[1]),
                                 static_cast<double>(header.min[0]), static_cast<double>(header.min[1]),
                                 static_cast<double>(header.max[0]), static_cast<double>(header.max[1]));
            }
            // The tight box is never larger.
            const box tight = shape->bounds();
            CHECK(tight.min[0] >= header.min[0] - 0.5f && tight.max[0] <= header.max[0] + 0.5f);
            CHECK(tight.min[1] >= header.min[1] - 0.5f && tight.max[1] <= header.max[1] + 0.5f);
        }
        std::printf("  %s: %zu outlines, %zu boxes differ from their header\n", f.family().c_str(), outlines,
                    mismatches);
        CHECK(outlines > 100);
        CHECK(mismatches <= allowed_mismatches);
    }

    void composites()
    {
        const font f = arial();

        // é is e (glyph 72) plus acute (141) moved by (243, 0).
        const auto composite = f.glyph_outline(112);
        const auto e = f.glyph_outline(72);
        const auto acute = f.glyph_outline(141);
        CHECK(composite && e && acute);
        if (!composite || !e || !acute)
            return;
        CHECK(composite->contour_count() == e->contour_count() + acute->contour_count());
        outline expected = *e;
        expected.append(*acute, affine::scale(1.0f, 1.0f, {243.0f, 0.0f}));
        CHECK(composite->segments().size() == expected.segments().size());
        bool same = composite->segments().size() == expected.segments().size();
        for (std::size_t i = 0; same && i < expected.segments().size(); ++i)
            same = composite->segments()[i].to == expected.segments()[i].to;
        CHECK(same);

        // Э (glyph 599) is З mirrored: a 2x2 transform with xscale -1.
        const auto mirrored = f.glyph_outline(599);
        CHECK(mirrored && !mirrored->empty());
        if (mirrored)
            CHECK(mirrored->control_bounds() == f.metrics(599).bounds);
    }

    void kerning()
    {
        struct pair
        {
            char32_t left, right;
            int arial, saira;
        };
        // From GPOS for both (Arial's 'kern' table agrees), via fontTools.
        const pair pairs[] = {
            {U'A', U'V', -152, -60}, {U'A', U'T', -152, -100}, {U'T', U'o', -227, -80},
            {U'L', U'T', -152, -100}, {U'Y', U'o', -188, -65}, {U'A', U'Y', -152, -70},
            {U'V', U'A', -152, -60}, {U'T', U'e', -227, -80}, {U'a', U'v', 0, -10},
            {U'a', U'b', 0, 0},       {U'P', U',', -264, 0},   {U'F', U'.', -227, -80},
            {U'r', U'.', -113, 0},    {U'A', U'W', -76, -60},  {U'L', U'y', -76, 0},
        };
        CHECK(arial().has_kerning());
        CHECK(saira().has_kerning());
        for (const pair &p : pairs)
        {
            CHECK(arial().kerning(arial().glyph(p.left), arial().glyph(p.right)) == p.arial);
            CHECK(saira().kerning(saira().glyph(p.left), saira().glyph(p.right)) == p.saira);
        }
        CHECK(arial().kerning(0, 0) == 0);
        CHECK(arial().kerning(60000, 60000) == 0);
    }

    // A collection made in memory from the two fonts: a 'ttcf' header, then
    // each font with its table offsets moved to where it now lies.
    std::vector<std::byte> make_collection()
    {
        const auto first = volt::tests::font::read_file(volt::tests::font::asset("DefaultFont.ttf"));
        const auto second = volt::tests::font::read_file(volt::tests::font::asset("SairaStencil.ttf"));

        std::vector<std::byte> out(20);
        const auto put32 = [&](std::size_t at, std::uint32_t value) {
            for (int i = 0; i < 4; ++i)
                out[at + static_cast<std::size_t>(i)] = static_cast<std::byte>((value >> (24 - 8 * i)) & 0xFFu);
        };
        const auto get16 = [&](std::size_t at) {
            return static_cast<std::uint32_t>((std::to_integer<std::uint32_t>(out[at]) << 8) |
                                              std::to_integer<std::uint32_t>(out[at + 1]));
        };
        const auto get32 = [&](std::size_t at) { return (get16(at) << 16) | get16(at + 2); };

        put32(0, 0x74746366u); // 'ttcf'
        put32(4, 0x00010000u);
        put32(8, 2);

        std::size_t slot = 12;
        for (const auto *font : {&first, &second})
        {
            while (out.size() % 4 != 0)
                out.push_back(std::byte{0});
            const std::size_t base = out.size();
            put32(slot, static_cast<std::uint32_t>(base));
            slot += 4;
            out.insert(out.end(), font->begin(), font->end());
            const std::uint32_t tables = get16(base + 4);
            for (std::uint32_t t = 0; t < tables; ++t)
            {
                const std::size_t record = base + 12 + t * 16 + 8;
                put32(record, get32(record) + static_cast<std::uint32_t>(base));
            }
        }
        return out;
    }

    void collections()
    {
        const std::vector<std::byte> bytes = make_collection();
        const auto count = font::face_count(bytes);
        CHECK(count && *count == 2);

        const auto all = font::load_collection(bytes);
        CHECK(all && all->size() == 2);
        if (all && all->size() == 2)
        {
            CHECK((*all)[0].family() == "Arial" && (*all)[0].index() == 0);
            CHECK((*all)[1].family() == "Saira Stencil" && (*all)[1].index() == 1);
            CHECK((*all)[0].glyph(U'A') == 36 && (*all)[1].glyph(U'A') == 1);
            CHECK((*all)[0].kerning(36, 57) == -152);
            const auto shape = (*all)[1].glyph_outline(1);
            CHECK(shape && shape->control_bounds() == saira().metrics(1).bounds);
        }

        const auto second = font::load(bytes, 1);
        CHECK(second && second->glyph_count() == 887);
        CHECK(!font::load(bytes, 2));
        // A plain font file has only face 0.
        CHECK(!font::load(volt::tests::font::read_file(volt::tests::font::asset("DefaultFont.ttf")), 1));
    }

    void failures()
    {
        CHECK(!font::load({}));
        const std::byte junk[64]{};
        CHECK(!font::load(junk));
        CHECK(!font::load_file("does/not/exist.ttf"));

        // An empty font is harmless.
        const font empty;
        CHECK(!empty.valid() && empty.id() == 0 && empty.glyph_count() == 0);
        CHECK(empty.glyph(U'A') == 0 && empty.advance(1) == 0 && empty.kerning(1, 2) == 0);
        CHECK(!empty.glyph_outline(0));
        CHECK(empty.family().empty());
    }
} // namespace

int main()
{
    arial_tables();
    saira_tables();
    cmap_and_metrics();
    trailing_metrics();
    outline_boxes(arial(), 0);
    outline_boxes(saira(), 0);
    composites();
    kerning();
    collections();
    failures();
    return volt::tests::result("font sfnt");
}
