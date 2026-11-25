#include "harness.hpp"

#include <ui/layout/box.hpp>
#include <ui/layout/grid.hpp>
#include <ui/painter.hpp>
#include <ui/theme.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/panel.hpp>
#include <ui/widgets/splitter.hpp>

// Measure and arrange: boxes with spacing, padding, flex and alignment;
// grids; labels measured with the real font; the scale factor.

using namespace volt::tests::ui;
using volt::math::vec2f;

namespace
{
    // A widget that wants a fixed size.
    class block : public widget
    {
    public:
        explicit block(vec2f size) : m_size(size) {}

    protected:
        vec2f on_measure(const constraints &) override { return m_size; }

    private:
        vec2f m_size;
    };

    void columns_and_rows()
    {
        harness h({400, 300});
        auto &root = h.screen->emplace_root<column>(10.0f, insets::uniform(5.0f));
        auto &a = root.emplace<block>(vec2f{100.0f, 20.0f});
        auto &b = root.emplace<block>(vec2f{50.0f, 30.0f});
        h.screen->layout();

        // Stretched across, stacked with spacing, inside the padding.
        CHECK(a.bounds() == (rect{5.0f, 5.0f, 390.0f, 20.0f}));
        CHECK(b.bounds() == (rect{5.0f, 35.0f, 390.0f, 30.0f}));
        CHECK(root.measured() == (vec2f{400.0f, 300.0f})); // the root fills the screen

        // Measured on its own, a column is the sum along and the widest
        // across, plus padding.
        const vec2f natural = root.measure(constraints::loose({1000.0f, 1000.0f}));
        CHECK(natural == (vec2f{110.0f, 70.0f}));
    }

    void flex_and_alignment()
    {
        harness h({300, 100});
        auto &root = h.screen->emplace_root<row>(0.0f);
        root.set_cross_align(align::center);
        auto &fixed = root.emplace<block>(vec2f{50.0f, 20.0f});
        auto &one = root.emplace<block>(vec2f{10.0f, 10.0f});
        auto &two = root.emplace<block>(vec2f{10.0f, 10.0f});
        one.layout().flex = 1.0f;
        two.layout().flex = 3.0f;
        two.layout().cross = align::end;
        h.screen->layout();

        // 250 left over, shared 1:3.
        CHECK(fixed.bounds() == (rect{0.0f, 40.0f, 50.0f, 20.0f}));
        CHECK(one.bounds().x == 50.0f);
        CHECK(near(one.bounds().width, 62.5f));
        CHECK(near(two.bounds().width, 187.5f));
        CHECK(near(two.bounds().right(), 300.0f));
        CHECK(two.bounds().bottom() == 100.0f); // its own alignment

        // Without flex, the main alignment places the group.
        one.layout().flex = 0.0f;
        two.layout().flex = 0.0f;
        root.set_main_align(align::end);
        h.screen->layout();
        CHECK(two.bounds().right() == 300.0f);
        CHECK(fixed.bounds().x == 300.0f - 70.0f);

        // Hidden children take no room.
        one.set_visible(false);
        h.screen->layout();
        CHECK(fixed.bounds().x == 300.0f - 60.0f);
        CHECK(one.bounds().empty());
    }

    void fixed_and_limited_sizes()
    {
        harness h({300, 100});
        auto &root = h.screen->emplace_root<row>();
        root.set_cross_align(align::start);
        auto &a = root.emplace<block>(vec2f{10.0f, 10.0f});
        a.layout().width = 80.0f;
        auto &b = root.emplace<block>(vec2f{500.0f, 500.0f});
        b.layout().max_size = {100.0f, 40.0f};
        auto &c = root.emplace<block>(vec2f{1.0f, 1.0f});
        c.layout().min_size = {20.0f, 20.0f};
        h.screen->layout();

        CHECK(a.bounds().width == 80.0f);
        CHECK(b.bounds().width == 100.0f);
        CHECK(b.bounds().height == 40.0f);
        CHECK(c.bounds().width == 20.0f);
        CHECK(c.bounds().height == 20.0f);
    }

    void grids()
    {
        harness h({400, 200});
        auto &g = h.screen->emplace_root<grid>(
            std::vector<grid_track>{grid_track::fixed(60.0f), grid_track::automatic(), grid_track::flex()}, 4.0f, 2.0f);
        auto &a = g.place(std::make_unique<block>(vec2f{30.0f, 20.0f}), 0, 0);
        auto &b = g.place(std::make_unique<block>(vec2f{70.0f, 24.0f}), 0, 1);
        auto &c = g.place(std::make_unique<block>(vec2f{10.0f, 10.0f}), 0, 2);
        auto &d = g.place(std::make_unique<block>(vec2f{10.0f, 30.0f}), 1, 0, 3);
        h.screen->layout();

        CHECK(g.column_widths().size() == 3);
        CHECK(g.column_widths()[0] == 60.0f);
        CHECK(g.column_widths()[1] == 70.0f);
        CHECK(g.column_widths()[2] == 400.0f - 60.0f - 70.0f - 8.0f);
        CHECK(g.row_heights()[0] == 24.0f);
        CHECK(g.row_heights()[1] == 30.0f);

        CHECK(a.bounds() == (rect{0.0f, 0.0f, 60.0f, 24.0f}));
        CHECK(b.bounds().x == 64.0f);
        CHECK(c.bounds().x == 138.0f);
        CHECK(d.bounds() == (rect{0.0f, 26.0f, 400.0f, 30.0f}));
    }

    void labels()
    {
        harness h({400, 200});
        auto &root = h.screen->emplace_root<column>();
        root.set_cross_align(align::start);
        auto &one = root.emplace<label>("Volt");
        auto &wrapped = root.emplace<label>("The quick brown fox jumps over the lazy dog, twice over.");
        wrapped.set_wrap(true);
        wrapped.layout().cross = align::stretch;
        h.screen->layout();

        // The width is the text's advance; the height a line.
        const text_style style = h.screen->theme().body_style();
        const float advance = h.text.measure("Volt", style);
        CHECK(one.bounds().width == std::ceil(advance));
        CHECK(one.bounds().height == std::ceil(h.text.metrics(style).line_height));

        // Narrow enough to break: more than one line.
        root.layout().width = 120.0f;
        h.screen->set_size({120, 200});
        h.screen->layout();
        CHECK(wrapped.bounds().height > 2.0f * one.bounds().height);
        CHECK(wrapped.bounds().width <= 120.0f);
    }

    void scale_factor()
    {
        // The same tree at scale 1 and 2: the same logical layout, twice
        // the pixels.
        for (float scale : {1.0f, 2.0f})
        {
            harness h({800, 600}, scale);
            auto &root = h.screen->emplace_root<column>(8.0f, insets::uniform(10.0f));
            root.set_cross_align(align::start);
            auto &a = root.emplace<block>(vec2f{100.0f, 20.0f});
            auto &l = root.emplace<label>("Scale");
            h.screen->layout();

            CHECK(h.screen->size() == (vec2f{800.0f / scale, 600.0f / scale}));
            CHECK(a.bounds() == (rect{10.0f, 10.0f, 100.0f, 20.0f}));

            // Painted, the block is scale times as many pixels...
            h.frame();

            draw_list list;
            painter p(list, h.text, h.screen->theme(), scale);
            p.fill(a.bounds(), {1.0f, 1.0f, 1.0f, 1.0f});
            CHECK(list.items()[0].bounds == (rect{10.0f * scale, 10.0f * scale, 100.0f * scale, 20.0f * scale}));

            // ...and the text's glyphs are rasterized at the scaled size.
            draw_list text_list;
            painter tp(text_list, h.text, h.screen->theme(), scale);
            tp.text(l.bounds(), "S", h.screen->theme().body_style(), {1.0f, 1.0f, 1.0f, 1.0f});
            CHECK(text_list.items().size() == 1);
            const float cap = text_list.items()[0].bounds.height;
            CHECK(cap >= 9.0f * scale && cap <= 12.0f * scale);

            // Hit testing is in logical units, whatever the scale.
            CHECK(h.screen->hit_test(a.bounds().center()) == &a);
        }
    }

    void splitter_layout()
    {
        harness h({600, 400});
        auto &s = h.screen->emplace_root<splitter>(orientation::horizontal);
        auto &left = s.set_first(std::make_unique<block>(vec2f{10.0f, 10.0f}));
        auto &right = s.set_second(std::make_unique<block>(vec2f{10.0f, 10.0f}));
        s.set_fixed(splitter::keep::first, 200.0f);
        h.screen->layout();
        const float gap = h.screen->theme().splitter_width;
        CHECK(left.bounds() == (rect{0.0f, 0.0f, 200.0f, 400.0f}));
        CHECK(right.bounds().x == 200.0f + gap);
        CHECK(right.bounds().right() == 600.0f);

        // The first pane keeps its size as the splitter grows.
        h.screen->set_size({900, 400});
        h.screen->layout();
        CHECK(left.bounds().width == 200.0f);
        CHECK(right.bounds().right() == 900.0f);

        // Minimums hold.
        s.set_minimums(50.0f, 100.0f);
        s.set_fixed(splitter::keep::first, 850.0f);
        h.screen->layout();
        CHECK(right.bounds().width == 100.0f);

        // A ratio grows both.
        s.set_ratio(0.25f);
        h.screen->layout();
        CHECK(near(left.bounds().width, std::round((900.0f - gap) * 0.25f)));
    }

    // A pane collapsed: hidden, the other fills the splitter, no divider to
    // grab, and the pane comes back where it was.
    void splitter_collapse()
    {
        harness h({900, 400});
        auto &s = h.screen->emplace_root<splitter>(orientation::horizontal);
        auto &left = s.set_first(std::make_unique<block>(vec2f{10.0f, 10.0f}));
        auto &right = s.set_second(std::make_unique<block>(vec2f{10.0f, 10.0f}));
        s.set_fixed(splitter::keep::second, 250.0f);
        s.set_minimums(100.0f, 200.0f);
        h.screen->layout();
        const float gap = h.screen->theme().splitter_width;
        CHECK(right.bounds().width == 250.0f);
        const vec2f divider = s.divider().center();
        CHECK(h.screen->hit_test(divider) == &s);

        s.set_collapsed(splitter::pane::second);
        CHECK(s.collapsed() == splitter::pane::second && s.is_collapsed(splitter::pane::second));
        h.screen->layout();
        CHECK(!right.visible());
        CHECK(left.bounds() == (rect{0.0f, 0.0f, 900.0f, 400.0f}));
        CHECK(s.divider().empty());
        CHECK(h.screen->hit_test(divider) == &left);
        CHECK(h.screen->hit_test({890.0f, 200.0f}) == &left);

        // Dragging where the divider was moves nothing.
        const int x = static_cast<int>(divider.x());
        h.drag(x, 200, x - 300, 200);
        h.screen->layout();
        CHECK(left.bounds().width == 900.0f);

        // Back as it was, minimums and all; growing the splitter meanwhile
        // kept the second pane's size.
        h.screen->set_size({1000, 400});
        s.set_collapsed(splitter::pane::none);
        h.screen->layout();
        CHECK(right.visible());
        CHECK(right.bounds().width == 250.0f);
        CHECK(left.bounds().width == 1000.0f - gap - 250.0f);
        CHECK(h.screen->hit_test(s.divider().center()) == &s);

        // Collapsing the first brings the second back; it fills it all.
        s.set_collapsed(splitter::pane::second);
        s.set_collapsed(splitter::pane::first);
        h.screen->layout();
        CHECK(!left.visible() && right.visible());
        CHECK(right.bounds() == (rect{0.0f, 0.0f, 1000.0f, 400.0f}));
        s.set_collapsed(splitter::pane::none);
        h.screen->layout();
        CHECK(left.visible() && right.bounds().width == 250.0f);
    }
} // namespace

int main()
{
    columns_and_rows();
    flex_and_alignment();
    fixed_and_limited_sizes();
    grids();
    labels();
    scale_factor();
    splitter_layout();
    splitter_collapse();
    return volt::tests::result("ui layout");
}
