#include "harness.hpp"

#include <ui/controllers.hpp>
#include <ui/widgets/image_view.hpp>
#include <ui/layout/box.hpp>
#include <ui/widgets/list_view.hpp>
#include <ui/widgets/property_grid.hpp>
#include <ui/widgets/splitter.hpp>
#include <ui/widgets/tab_view.hpp>
#include <ui/widgets/text_field.hpp>
#include <ui/widgets/viewport.hpp>

#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <vector>

// Views of many things and of other things: virtualized lists and trees,
// tabs, the splitter dragged, the property grid, the viewport with the two
// camera controllers, and an image fitted into a viewer.

using namespace volt::tests::ui;
using volt::math::vec2f;
using volt::math::vec3f;

namespace
{
    std::vector<std::string> numbered(int count)
    {
        std::vector<std::string> items;
        for (int i = 0; i < count; ++i)
            items.push_back(std::format("Item {}", i));
        return items;
    }

    void virtualized_list()
    {
        harness h({300, 220});
        auto &list = h.screen->emplace_root<list_view>(numbered(10000));
        h.screen->layout();
        const float row = list.row_height();

        // 220 high less the frame: only about ten rows are visible...
        auto [first, last] = list.visible_range();
        CHECK(first == 0);
        CHECK(last == static_cast<std::size_t>(std::ceil(218.0f / row)));

        // ...and only they are painted, whatever the count.
        h.frame();
        CHECK(list.painted_rows() == last - first);
        CHECK(count(h.list, shape_kind::glyph) < 200);

        // Scrolled far down, the range follows.
        list.scroll_to(5000.0f * row);
        std::tie(first, last) = list.visible_range();
        CHECK(first == 5000);
        CHECK(last - first <= 11);
        h.frame();
        CHECK(list.painted_rows() == last - first);

        // Clicking selects the row under the pointer.
        int selected = -1;
        list.selection_changed.connect([&](int i) { selected = i; });
        const rect r = list.row_rect(5002);
        h.click(static_cast<int>(r.center().x()), static_cast<int>(r.center().y()));
        CHECK(list.selected() == 5002);
        CHECK(selected == 5002);

        // The keyboard moves it and scrolls it into view.
        h.key(key::end);
        CHECK(list.selected() == 9999);
        std::tie(first, last) = list.visible_range();
        CHECK(last == 10000);
        h.key(key::home);
        CHECK(list.selected() == 0);
        CHECK(list.scroll_offset() == 0.0f);
        h.key(key::page_down);
        CHECK(list.selected() > 5);

        // Double click or Enter activates.
        int activated = -1;
        list.activated.connect([&](int i) { activated = i; });
        h.key(key::enter);
        CHECK(activated == list.selected());

        // The wheel scrolls three rows a notch.
        list.scroll_to(0.0f);
        h.scroll(100, 100, -1.0);
        CHECK(list.scroll_offset() == 3.0f * row);
    }

    // A list told to show a row before it is shown (a panel built with
    // its selection kept from the last time) has no theme to measure rows
    // with yet: it scrolls to the row once it is laid out.
    void scrolled_before_shown()
    {
        harness h({300, 220});
        auto made = std::make_unique<list_view>(numbered(1000));
        list_view &list = *made;
        list.set_selected(700);
        list.ensure_visible(700);
        CHECK(list.selected() == 700);
        CHECK(list.scroll_offset() == 0.0f);

        h.screen->set_root(std::move(made));
        h.screen->layout();
        auto [first, last] = list.visible_range();
        CHECK(first <= 700 && 700 < last);

        // Asked for no row, it forgets the one it was waiting to show.
        auto other = std::make_unique<list_view>(numbered(1000));
        list_view &unmoved = *other;
        unmoved.ensure_visible(700);
        unmoved.set_selected(-1);
        h.screen->set_root(std::move(other));
        h.screen->layout();
        CHECK(unmoved.scroll_offset() == 0.0f);
    }

    tree_node leaf(std::string text, std::vector<tree_node> children = {})
    {
        tree_node n;
        n.text = std::move(text);
        n.children = std::move(children);
        return n;
    }

    std::vector<tree_node> sample_tree()
    {
        return {leaf("A", {leaf("A1"), leaf("A2")}), leaf("B", {leaf("B1", {leaf("B1x")})}), leaf("C")};
    }

    void tree()
    {
        harness h({300, 300});
        auto &t = h.screen->emplace_root<tree_view>(sample_tree());
        h.screen->layout();
        CHECK(t.row_count() == 3);

        // Click the arrow: opens.
        h.frame();
        const rect a = t.row_rect(0);
        const float arrow_x = a.x + 4.0f + h.screen->theme().indent * 0.5f;
        h.click(static_cast<int>(arrow_x), static_cast<int>(a.center().y()));
        CHECK(t.row_count() == 5);
        CHECK(t.node(1).text == "A1");
        CHECK(t.depth(1) == 1);

        // Keyboard: select B, Right opens, Right again goes to its child,
        // Left goes back to the parent, Left closes.
        t.focus();
        t.set_selected(3);
        CHECK(t.selected_node()->text == "B");
        h.key(key::right);
        CHECK(t.row_count() == 6);
        h.key(key::right);
        CHECK(t.selected_node()->text == "B1");
        h.key(key::left);
        CHECK(t.selected_node()->text == "B");
        h.key(key::left);
        CHECK(t.row_count() == 5);
        CHECK(t.selected_node()->text == "B");

        // Double click a folder: toggles.
        const rect b = t.row_rect(3);
        h.double_click(static_cast<int>(b.center().x()), static_cast<int>(b.center().y()));
        CHECK(t.row_count() == 6);

        // The selection stays on its node as rows come and go above it.
        t.set_selected(3);
        t.set_expanded(0, false);
        CHECK(t.selected_node()->text == "B");
        CHECK(t.selected() == 1);

        t.expand_all();
        CHECK(t.row_count() == 7);
    }

    void tabs()
    {
        harness h({400, 300});
        auto &tabs = h.screen->emplace_root<tab_view>();
        auto &one = tabs.add_tab("One", std::make_unique<text_field>("1"));
        auto &two = tabs.add_tab("Two", std::make_unique<text_field>("2"));
        h.screen->layout();
        CHECK(tabs.selected() == 0);
        CHECK(one.visible() && !two.visible());

        int changed = -1;
        tabs.changed.connect([&](int i) { changed = i; });
        h.frame();
        const rect head = tabs.tab_rect(1);
        h.click(static_cast<int>(head.center().x()), static_cast<int>(head.center().y()));
        CHECK(tabs.selected() == 1 && changed == 1);
        CHECK(!one.visible() && two.visible());
        h.screen->layout();
        CHECK(two.bounds().y >= h.screen->theme().tab_height);

        two.focus();
        h.key(key::page_up, modifiers::control);
        CHECK(tabs.selected() == 0);
    }

    void splitter_drag()
    {
        harness h({600, 300});
        auto &s = h.screen->emplace_root<splitter>(orientation::horizontal);
        auto &left = s.set_first(std::make_unique<list_view>());
        s.set_second(std::make_unique<list_view>());
        s.set_fixed(splitter::keep::first, 200.0f);
        h.screen->layout();

        float moved = 0.0f;
        s.moved.connect([&](float p) { moved = p; });
        const rect d = s.divider();
        CHECK(h.screen->hit_test(d.center()) == &s);
        h.move(static_cast<int>(d.center().x()), 150);
        CHECK(h.screen->cursor() == cursor_shape::resize_horizontal);

        h.drag(static_cast<int>(d.center().x()), 150, static_cast<int>(d.center().x()) + 100, 150);
        CHECK(near(s.position(), 300.0f, 1.0f));
        CHECK(near(moved, 300.0f, 1.0f));
        CHECK(near(left.bounds().width, 300.0f, 1.0f));

        // Dragged into the other pane's minimum, it stops.
        s.set_minimums(50.0f, 150.0f);
        h.drag(static_cast<int>(s.divider().center().x()), 150, 590, 150);
        CHECK(near(s.position(), 600.0f - h.screen->theme().splitter_width - 150.0f, 1.0f));

        // The kept size stays as the window grows.
        h.screen->set_size({800, 300});
        h.screen->layout();
        CHECK(near(left.bounds().width, s.position()));
    }

    void property_rows()
    {
        harness h({300, 400});
        auto &grid = h.screen->emplace_root<property_grid>();
        grid.add_section("First");
        auto &a = grid.add_row("Name", std::make_unique<text_field>("x"));
        grid.add_section("Second");
        auto &b = grid.add_row("Other", std::make_unique<text_field>("y"));
        h.screen->layout();

        CHECK(a.bounds().x > 100.0f);       // the value column
        CHECK(b.bounds().y > a.bounds().bottom());
        CHECK(a.bounds().right() <= 300.0f);

        // Folding a section hides its editors.
        grid.set_section_expanded(0, false);
        h.screen->layout();
        CHECK(!a.visible());
        CHECK(b.bounds().y < 80.0f);

        // Tab goes through the editors in row order.
        grid.set_section_expanded(0, true);
        h.screen->layout();
        h.key(key::tab);
        CHECK(h.screen->focused() == &a);
        h.key(key::tab);
        CHECK(h.screen->focused() == &b);
    }

    void pan_zoom()
    {
        harness h({400, 300}, 2.0f);
        auto &v = h.screen->emplace_root<viewport>(texture_id::image(0));
        pan_zoom_controller c;
        c.zoom = 10.0f;
        v.set_controller(&c);
        int changes = 0;
        v.changed.connect([&] { ++changes; });
        h.screen->layout();

        // The viewport's pixel size is the screen's pixels.
        CHECK(v.pixel_size() == (volt::math::vec2i{400, 300}));
        const vec2f size{400.0f, 300.0f};

        // Pixel and world round trip, y flipped.
        CHECK(c.to_world({200.0f, 150.0f}, size) == (vec2f{0.0f, 0.0f}));
        CHECK(c.to_world({210.0f, 140.0f}, size) == (vec2f{1.0f, 1.0f}));
        CHECK(c.to_pixels({1.0f, 1.0f}, size) == (vec2f{210.0f, 140.0f}));

        // Wheel zoom about the pointer keeps the point under it.
        const vec2f anchor = c.to_world({300.0f, 100.0f}, size);
        h.scroll(300, 100, 2.0);
        CHECK(c.zoom > 10.0f);
        const vec2f after = c.to_world({300.0f, 100.0f}, size);
        CHECK(near(after.x(), anchor.x(), 1e-3f) && near(after.y(), anchor.y(), 1e-3f));
        CHECK(changes == 1);

        // A middle drag pans: the world moves with the pointer.
        const vec2f grabbed = c.to_world({100.0f, 100.0f}, size);
        h.drag(100, 100, 160, 140, mouse_button::middle);
        const vec2f now_there = c.to_world({160.0f, 140.0f}, size);
        CHECK(near(now_there.x(), grabbed.x(), 1e-3f) && near(now_there.y(), grabbed.y(), 1e-3f));

        // A left drag does nothing to this camera.
        const vec2f center = c.center;
        h.drag(100, 100, 150, 150, mouse_button::left);
        CHECK(c.center == center);

        // Home fits.
        c.fit_min = {-10.0f, -5.0f};
        c.fit_max = {10.0f, 5.0f};
        v.focus();
        h.key(key::home);
        CHECK(c.center == (vec2f{0.0f, 0.0f}));
        CHECK(near(c.zoom, 0.9f * 400.0f / 20.0f));

        // A right click without a drag asks for a context menu.
        int context = 0;
        v.context_requested.connect([&](vec2f) { ++context; });
        h.click(50, 50, mouse_button::right);
        CHECK(context == 1);
    }

    void orbit()
    {
        orbit_controller c;
        c.look({0.0f, -100.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
        CHECK(near(c.distance, 100.0f));
        const vec3f eye = c.eye();
        CHECK(near(eye.x(), 0.0f, 1e-3f) && near(eye.y(), -100.0f, 1e-3f) && near(eye.z(), 0.0f, 1e-3f));

        viewport_pointer p;
        p.size = {400.0f, 300.0f};
        p.button = mouse_button::left;
        p.buttons = mask_of(mouse_button::left);
        c.pointer_down(p);
        p.delta = {0.0f, 50.0f};
        CHECK(c.pointer_move(p));
        CHECK(c.pitch > 0.0f); // dragging down tilts the eye up
        CHECK(near(c.distance, 100.0f));
        p.buttons = mouse_buttons::none;
        c.pointer_up(p);

        // Pitch stops short of the poles.
        p.buttons = mask_of(mouse_button::left);
        c.pointer_down(p);
        p.delta = {0.0f, 100000.0f};
        c.pointer_move(p);
        CHECK(c.pitch < 1.5708f);
        p.buttons = mouse_buttons::none;
        c.pointer_up(p);

        // Panning moves the target, not the distance.
        c.look({0.0f, -100.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
        p.button = mouse_button::middle;
        p.buttons = mask_of(mouse_button::middle);
        c.pointer_down(p);
        p.delta = {10.0f, 0.0f};
        c.pointer_move(p);
        CHECK(c.target.x() < 0.0f); // the scene follows the pointer right: the target goes left
        CHECK(near(c.distance, 100.0f));
        p.buttons = mouse_buttons::none;
        c.pointer_up(p);

        // The wheel dollies in, within limits.
        CHECK(c.scroll({0.0f, 1.0f}, p));
        CHECK(c.distance < 100.0f);
        for (int i = 0; i < 100; ++i)
            c.scroll({0.0f, 1.0f}, p);
        CHECK(c.distance == c.min_distance);
    }

    // The image item a list holds, if exactly one.
    const draw_item *only_image(const draw_list &list)
    {
        const draw_item *found = nullptr;
        for (const draw_item &item : list.items())
        {
            if (item.kind != shape_kind::image)
                continue;
            if (found != nullptr)
                return nullptr;
            found = &item;
        }
        return found;
    }

    void image_fit()
    {
        // A wide image in a square: as wide as the square, its height by
        // the aspect ratio, centred; never larger than its own pixels.
        {
            harness h({200, 200});
            auto &view = h.screen->emplace_root<image_view>(texture_id::image(3), vec2f{10.0f, 10.0f});
            view.set_fit({800.0f, 200.0f});
            const draw_item *item = only_image(h.frame());
            CHECK(item != nullptr);
            if (item != nullptr)
            {
                CHECK(item->texture == texture_id::image(3));
                CHECK(std::abs(item->bounds.width - 200.0f) < 0.5f);
                CHECK(std::abs(item->bounds.height - 50.0f) < 0.5f);
                CHECK(std::abs(item->bounds.y - 75.0f) < 0.5f);
            }
        }

        // A small one stays its size, in pixels, at scale 2 as well.
        {
            harness h({300, 300}, 2.0f);
            auto &view = h.screen->emplace_root<image_view>(texture_id::image(0), vec2f{10.0f, 10.0f});
            view.set_fit({64.0f, 32.0f});
            const draw_item *item = only_image(h.frame());
            CHECK(item != nullptr);
            if (item != nullptr)
            {
                CHECK(std::abs(item->bounds.width - 64.0f) < 0.5f);
                CHECK(std::abs(item->bounds.height - 32.0f) < 0.5f);
                CHECK(std::abs(item->bounds.x - 118.0f) < 0.5f);
            }

            // Asked to, it fills the width.
            view.set_fit({64.0f, 32.0f}, true);
            item = only_image(h.frame());
            CHECK(item != nullptr && std::abs(item->bounds.width - 300.0f) < 0.5f);
        }

        // The checkerboard goes under the image, inside its rectangle.
        {
            harness h({100, 100});
            auto &view = h.screen->emplace_root<image_view>(texture_id::image(1), vec2f{10.0f, 10.0f});
            view.set_fit({40.0f, 20.0f});
            view.set_checkerboard(true);
            const draw_list &list = h.frame();
            CHECK(count(list, shape_kind::rectangle) > 4);
            CHECK(!list.items().empty() && list.items().back().kind == shape_kind::image);
            for (const draw_item &item : list.items())
            {
                CHECK(item.bounds.x >= 30.0f - 0.01f && item.bounds.right() <= 70.0f + 0.01f);
                CHECK(item.bounds.y >= 40.0f - 0.01f && item.bounds.bottom() <= 60.0f + 0.01f);
            }
        }
    }
} // namespace

int main()
{
    virtualized_list();
    scrolled_before_shown();
    tree();
    tabs();
    splitter_drag();
    property_rows();
    pan_zoom();
    orbit();
    image_fit();
    return volt::tests::result("ui views");
}
