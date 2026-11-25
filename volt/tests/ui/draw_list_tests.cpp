#include "harness.hpp"

#include <ui/painter.hpp>
#include <ui/theme.hpp>

// The draw list on its own: what each call adds, clipping and its nesting,
// culling, texture ids; and the painter's units: snapping and scale.

using namespace volt::tests::ui;
using volt::math::vec2f;
using volt::math::vec4f;

namespace
{
    const vec4f white{1.0f, 1.0f, 1.0f, 1.0f};

    void shapes()
    {
        draw_list list;
        list.rectangle({10.0f, 20.0f, 30.0f, 40.0f}, white, 4.0f);
        list.rectangle({0.0f, 0.0f, 10.0f, 10.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, 2.0f, 1.0f, white); // an outline
        list.shadow({0.0f, 0.0f, 10.0f, 10.0f}, 2.0f, 6.0f, white);
        list.line({0.0f, 0.0f}, {10.0f, 10.0f}, 2.0f, white);
        const vec2f points[] = {{0.0f, 0.0f}, {5.0f, 5.0f}, {10.0f, 0.0f}};
        list.polyline(points, 1.5f, white);
        list.image({0.0f, 0.0f, 16.0f, 16.0f}, texture_id::image(3));
        list.glyph({1.0f, 2.0f, 3.0f, 4.0f}, texture_id::glyph_page(1), {0.1f, 0.2f}, {0.3f, 0.4f}, white, 0.25f);

        CHECK(list.items().size() == 8);
        CHECK(count(list, shape_kind::rectangle) == 2);
        CHECK(count(list, shape_kind::shadow) == 1);
        CHECK(count(list, shape_kind::line) == 3); // a polyline is lines
        CHECK(count(list, shape_kind::image) == 1);
        CHECK(count(list, shape_kind::glyph) == 1);

        const draw_item &first = list.items()[0];
        CHECK(first.bounds == rect{10.0f, 20.0f, 30.0f, 40.0f});
        CHECK(first.radius == 4.0f);
        CHECK(first.clip == 0);

        const draw_item &outline = list.items()[1];
        CHECK(outline.border == 1.0f);
        CHECK(outline.border_color == white);

        const draw_item &image = list.items()[6];
        CHECK(image.texture == texture_id::image(3));
        CHECK(image.texture.kind == texture_kind::image);

        const draw_item &glyph = list.items()[7];
        CHECK(glyph.texture == texture_id::glyph_page(1));
        CHECK(glyph.uv_min == vec2f{0.1f, 0.2f});
        CHECK(glyph.uv_max == vec2f{0.3f, 0.4f});
        CHECK(glyph.extra == 0.25f);

        // Nothing to see, nothing added.
        draw_list empty;
        empty.rectangle({0.0f, 0.0f, 0.0f, 10.0f}, white);
        empty.rectangle({0.0f, 0.0f, 10.0f, 10.0f}, {1.0f, 1.0f, 1.0f, 0.0f});
        empty.image({0.0f, 0.0f, 10.0f, 10.0f}, texture_id{});
        empty.line({0.0f, 0.0f}, {1.0f, 1.0f}, 0.0f, white);
        CHECK(empty.empty());

        list.clear();
        CHECK(list.empty());
        CHECK(list.clips().size() == 1);
        CHECK(list.clip_depth() == 0);
    }

    void clips()
    {
        draw_list list;
        list.push_clip({0.0f, 0.0f, 100.0f, 100.0f});
        list.rectangle({10.0f, 10.0f, 10.0f, 10.0f}, white);

        // Nested: the intersection.
        list.push_clip({50.0f, 50.0f, 100.0f, 100.0f});
        CHECK(list.current_clip() == (rect{50.0f, 50.0f, 50.0f, 50.0f}));
        CHECK(list.clip_depth() == 2);
        list.rectangle({60.0f, 60.0f, 10.0f, 10.0f}, white);

        // Entirely outside the current clip: culled.
        list.rectangle({10.0f, 10.0f, 10.0f, 10.0f}, white);
        CHECK(list.items().size() == 2);

        // A line whose box touches the clip only through its width is kept.
        list.line({45.0f, 40.0f}, {45.0f, 60.0f}, 12.0f, white);
        CHECK(list.items().size() == 3);

        list.pop_clip();
        CHECK(list.current_clip() == (rect{0.0f, 0.0f, 100.0f, 100.0f}));
        list.rectangle({10.0f, 10.0f, 10.0f, 10.0f}, white);

        // A clip that does not meet the current one hides everything.
        list.push_clip({200.0f, 200.0f, 10.0f, 10.0f});
        CHECK(list.current_clip().empty());
        list.rectangle({200.0f, 200.0f, 5.0f, 5.0f}, white);
        list.pop_clip();
        list.pop_clip();
        CHECK(list.clip_depth() == 0);

        const auto items = list.items();
        CHECK(items.size() == 4);
        CHECK(list.clips()[items[0].clip] == (rect{0.0f, 0.0f, 100.0f, 100.0f}));
        CHECK(list.clips()[items[1].clip] == (rect{50.0f, 50.0f, 50.0f, 50.0f}));
        CHECK(list.clips()[items[3].clip] == (rect{0.0f, 0.0f, 100.0f, 100.0f}));
    }

    void painter_units()
    {
        harness h;
        const theme t = theme::dark_theme();

        // Scale 1.5: edges go to the nearest pixel, so neighbours stay
        // neighbours.
        {
            draw_list list;
            painter p(list, h.text, t, 1.5f);
            p.fill({1.0f, 1.0f, 3.0f, 3.0f}, white);
            p.fill({4.0f, 1.0f, 3.0f, 3.0f}, white);
            const rect a = list.items()[0].bounds;
            const rect b = list.items()[1].bounds;
            CHECK(a.x == 2.0f);       // round(1.5)
            CHECK(a.right() == 6.0f); // round(6.0)
            CHECK(b.x == a.right());
            CHECK(b.right() == 11.0f); // round(10.5)
        }

        // Borders are at least a pixel, and scale.
        {
            draw_list list;
            painter p(list, h.text, t, 2.0f);
            p.frame({0.0f, 0.0f, 10.0f, 10.0f}, white, white, 3.0f, 1.0f);
            CHECK(list.items()[0].border == 2.0f);
            CHECK(list.items()[0].radius == 6.0f);
            CHECK(list.items()[0].bounds == (rect{0.0f, 0.0f, 20.0f, 20.0f}));

            painter thin(list, h.text, t, 0.5f);
            thin.outline({0.0f, 0.0f, 10.0f, 10.0f}, white, 0.0f, 1.0f);
            CHECK(list.items()[1].border == 1.0f);
        }

        // The clip pushed in logical units comes back as logical units.
        {
            draw_list list;
            painter p(list, h.text, t, 2.0f);
            p.push_clip({10.0f, 10.0f, 20.0f, 20.0f});
            CHECK(list.current_clip() == (rect{20.0f, 20.0f, 40.0f, 40.0f}));
            CHECK(p.visible() == (rect{10.0f, 10.0f, 20.0f, 20.0f}));
            p.pop_clip();
        }
    }

    void text_glyphs()
    {
        harness h;
        const theme t = theme::dark_theme();

        // Text becomes glyph quads on the atlas's pages, on whole pixels,
        // one per visible character (spaces have none).
        draw_list list;
        painter p(list, h.text, t, 1.0f);
        p.text({0.0f, 0.0f, 300.0f, 30.0f}, "Volt UI", t.body_style(), t.text);
        CHECK(count(list, shape_kind::glyph) == 6);
        for (const draw_item &item : list.items())
        {
            CHECK(item.texture.kind == texture_kind::glyphs);
            CHECK(item.bounds.x == std::floor(item.bounds.x));
            CHECK(item.bounds.y == std::floor(item.bounds.y));
        }
        CHECK(h.text.atlas().page_count() >= 1);

        // At twice the scale the glyphs are twice as big: rasterized for
        // the pixels, not stretched.
        draw_list big;
        painter p2(big, h.text, t, 2.0f);
        p2.text({0.0f, 0.0f, 300.0f, 30.0f}, "V", t.body_style(), t.text);
        CHECK(big.items().size() == 1);
        CHECK(list.items().size() >= 1);
        const float small_height = list.items()[0].bounds.height;
        const float big_height = big.items()[0].bounds.height;
        CHECK(big_height >= 2.0f * small_height - 2.0f && big_height <= 2.0f * small_height + 2.0f);

        // Text too long for its box ends in an ellipsis within the box.
        draw_list cut;
        painter p3(cut, h.text, t, 1.0f);
        p3.text({0.0f, 0.0f, 60.0f, 20.0f}, "A rather long piece of text", t.body_style(), t.text);
        float right = 0.0f;
        for (const draw_item &item : cut.items())
            right = std::max(right, item.bounds.right());
        CHECK(right <= 61.0f);
        CHECK(count(cut, shape_kind::glyph) < 20);
    }

    void atlas_overflow()
    {
        // A tiny atlas fills up; the screen paints again after a reset, so
        // the list it hands out only names glyphs the atlas still has.
        harness h;
        volt::font::atlas_options options;
        options.page_size = 64;
        options.max_pages = 1;
        font_text_engine small(options);
        small.add(volt::font::font_chain({default_font()}));
        screen s(small);
        s.set_size({400, 100});

        class many : public widget
        {
        protected:
            void on_paint(painter &p) override
            {
                const auto &t = this->theme();
                // Several sizes of the alphabet, far more than 64x64 holds.
                for (int i = 0; i < 4; ++i)
                    p.text({0.0f, 20.0f * static_cast<float>(i), 400.0f, 20.0f}, "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                           {t.font, 10.0f + 3.0f * static_cast<float>(i)}, t.text);
            }
        };
        s.emplace_root<many>();
        draw_list list;
        s.paint(list);
        // Whatever made it into the list was placed after the reset: on the
        // one page, which the atlas still has.
        CHECK(count(list, shape_kind::glyph) > 0);
        CHECK(small.atlas().page_count() == 1);
        for (const draw_item &item : list.items())
            CHECK(item.texture == texture_id::glyph_page(0));
    }
} // namespace

int main()
{
    shapes();
    clips();
    painter_units();
    text_glyphs();
    atlas_overflow();
    return volt::tests::result("ui draw list");
}
