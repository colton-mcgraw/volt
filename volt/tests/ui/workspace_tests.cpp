#include "harness.hpp"

#include <ui/layout/box.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/log_view.hpp>
#include <ui/widgets/tile_grid.hpp>
#include <ui/widgets/tool_strip.hpp>

#include <format>
#include <string>

// The widgets an editor window is arranged with: the tool strip beside a
// canvas, the log of what happened, and the grid of tiles that wraps with
// the window's width.

using namespace volt::tests::ui;

namespace
{
    void strip_of_tools()
    {
        harness h({300, 400});
        auto &body = h.screen->emplace_root<row>();
        auto &strip = body.emplace<tool_strip>(orientation::vertical);
        const int select = strip.add_tool(icon::pointer, "Select (Esc)");
        strip.add_separator();
        const int wire = strip.add_tool(icon::wire, "Wire (W)");
        const int text = strip.add_tool(icon::text, "Text (T)");
        body.emplace<spacer>();
        h.frame();

        // As wide as a toolbar is high; the tools one above the other, in
        // the order added, the first one active.
        CHECK(strip.tool_count() == 3);
        CHECK(strip.bounds().width >= h.screen->theme().toolbar_height - 0.01f);
        CHECK(strip.tool(0).bounds().y < strip.tool(1).bounds().y);
        CHECK(strip.tool(1).bounds().y < strip.tool(2).bounds().y);
        CHECK(strip.selected() == select);
        CHECK(strip.tool(0).checked() && !strip.tool(1).checked() && !strip.tool(2).checked());

        // Tools never take the focus from the document.
        for (std::size_t i = 0; i < strip.tool_count(); ++i)
            CHECK(!strip.tool(i).focusable());

        int changed = -1;
        int emitted = 0;
        strip.changed.connect([&](int index) {
            changed = index;
            ++emitted;
        });

        // A click makes a tool the active one, and only that one down.
        h.click_on(strip.tool(static_cast<std::size_t>(wire)));
        CHECK(strip.selected() == wire && changed == wire && emitted == 1);
        CHECK(!strip.tool(0).checked() && strip.tool(1).checked() && !strip.tool(2).checked());

        // Clicking the active one again leaves it down and says nothing.
        h.click_on(strip.tool(static_cast<std::size_t>(wire)));
        CHECK(strip.selected() == wire && emitted == 1);
        CHECK(strip.tool(1).checked());

        // select() is for the window's own shortcuts: no signal; nonsense
        // is ignored.
        strip.select(text);
        CHECK(strip.selected() == text && emitted == 1);
        CHECK(strip.tool(2).checked() && !strip.tool(1).checked());
        strip.select(7);
        strip.select(-1);
        CHECK(strip.selected() == text);

        // Horizontal: named choices side by side.
        harness g({500, 100});
        auto &column_root = g.screen->emplace_root<column>();
        auto &sheets = column_root.emplace<tool_strip>(orientation::horizontal);
        sheets.add_tool(icon::sheet, "First sheet", "First");
        sheets.add_tool(icon::sheet, "Second sheet", "Second");
        g.frame();
        CHECK(sheets.tool(0).bounds().right() <= sheets.tool(1).bounds().x);
        CHECK(near(sheets.tool(0).bounds().y, sheets.tool(1).bounds().y));
        CHECK(sheets.bounds().height < 60.0f);
    }

    log_entry entry(std::string text, log_level level = log_level::info, std::string source = "Board Editor")
    {
        return {"12:00:00", std::move(source), std::move(text), level};
    }

    void running_log()
    {
        // Repeats count on the last row instead of adding one.
        {
            harness h({400, 200});
            auto &log = h.screen->emplace_root<log_view>();
            log.append(entry("Loaded a.step"));
            log.append(entry("Saved"));
            log.append(entry("Saved"));
            log.append(entry("Saved", log_level::info, "Schematic Editor")); // elsewhere: a row of its own
            CHECK(log.row_count() == 3);
            CHECK(log.entries()[1].count == 2);
            CHECK(log.entries()[2].count == 1);
            CHECK(log_view::line(log.entries()[1]) == "12:00:00  Board Editor  Saved (\xC3\x97" "2)");
            CHECK(log_view::line(entry("Cannot open x", log_level::error, "")) == "12:00:00  Error:  Cannot open x");
        }

        // The oldest go once there are more than it keeps.
        {
            harness h({400, 200});
            auto &log = h.screen->emplace_root<log_view>(5);
            for (int i = 0; i < 8; ++i)
                log.append(entry(std::format("Message {}", i)));
            CHECK(log.row_count() == 5);
            CHECK(log.entries().front().text == "Message 3");
            CHECK(log.entries().back().text == "Message 7");
        }

        // It follows the end while the end is in view, and stays put while
        // the user reads further up.
        {
            harness h({400, 120});
            auto &log = h.screen->emplace_root<log_view>();
            for (int i = 0; i < 50; ++i)
                log.append(entry(std::format("Message {}", i)));
            h.frame();
            CHECK(log.visible_range().second == 50);

            log.append(entry("Message 50"));
            h.frame();
            CHECK(log.visible_range().second == 51);

            log.scroll_to(0.0f);
            h.frame();
            log.append(entry("Message 51"));
            h.frame();
            CHECK(log.scroll_offset() == 0.0f);
            CHECK(log.visible_range().first == 0);

            log.scroll_to(1.0e6f);
            h.frame();
            log.append(entry("Message 52"));
            h.frame();
            CHECK(log.visible_range().second == 53);
        }

        // Ctrl+C copies the selected entry; with nothing selected, nothing.
        {
            harness h({400, 200});
            auto &log = h.screen->emplace_root<log_view>();
            log.append(entry("First"));
            log.append(entry("Cannot open b.step", log_level::error));
            h.frame();
            h.screen->clipboard().set_text("before");
            log.focus();
            h.key(key::c, modifiers::control);
            CHECK(h.screen->clipboard().text() == "before");

            const rect r = log.row_rect(1);
            h.click(static_cast<int>(r.center().x()), static_cast<int>(r.center().y()));
            CHECK(log.selected() == 1);
            h.key(key::c, modifiers::control);
            CHECK(h.screen->clipboard().text() == "12:00:00  Board Editor  Error:  Cannot open b.step");
        }

        // An error is painted in the theme's error colour; an empty log
        // shows its placeholder, and only then.
        {
            harness h({400, 200});
            auto &log = h.screen->emplace_root<log_view>();
            const draw_list &empty = h.frame();
            CHECK(count(empty, shape_kind::glyph) == 0);
            log.set_placeholder("Nothing yet");
            CHECK(count(h.frame(), shape_kind::glyph) > 0);

            log.append(entry("Bad", log_level::error, ""));
            const draw_list &list = h.frame();
            const volt::math::vec4f error = h.screen->theme().error;
            bool painted_error = false;
            for (const draw_item &item : list.items())
                painted_error = painted_error || (item.kind == shape_kind::glyph && item.color == error);
            CHECK(painted_error);

            log.clear();
            CHECK(log.row_count() == 0);
        }
    }

    // A tile of a fixed height, or a wrapping text.
    label &fixed_tile(widget &parent, float height)
    {
        auto &tile = parent.emplace<label>("Tile");
        tile.layout().height = height;
        return tile;
    }

    void tiles()
    {
        // Wide: all three in one row, sharing the width and the tallest
        // height.
        {
            harness h({800, 400});
            auto &body = h.screen->emplace_root<column>();
            auto &grid = body.emplace<tile_grid>(180.0f, 8.0f);
            auto &a = fixed_tile(grid, 50.0f);
            auto &b = fixed_tile(grid, 80.0f);
            auto &c = fixed_tile(grid, 60.0f);
            h.frame();
            CHECK(grid.columns() == 3);
            CHECK(near(a.bounds().width, 261.0f) && near(b.bounds().width, 261.0f));
            CHECK(near(a.bounds().y, b.bounds().y) && near(b.bounds().y, c.bounds().y));
            CHECK(near(a.bounds().height, 80.0f) && near(c.bounds().height, 80.0f));
            CHECK(near(b.bounds().x, a.bounds().right() + 8.0f));
            CHECK(near(grid.bounds().height, 80.0f));

            // Narrower: two to a row, the third below.
            h.screen->set_size({400, 400});
            h.frame();
            CHECK(grid.columns() == 2);
            CHECK(near(a.bounds().y, b.bounds().y));
            CHECK(near(c.bounds().y, a.bounds().bottom() + 8.0f));
            CHECK(near(c.bounds().x, a.bounds().x));
            CHECK(near(c.bounds().height, 60.0f));
            CHECK(near(grid.bounds().height, 80.0f + 8.0f + 60.0f));

            // Narrower than a tile: one column, as wide as there is.
            h.screen->set_size({150, 400});
            h.frame();
            CHECK(grid.columns() == 1);
            CHECK(near(a.bounds().width, 150.0f));
            CHECK(b.bounds().y > a.bounds().y && c.bounds().y > b.bounds().y);

            // Hidden tiles take no place.
            h.screen->set_size({800, 400});
            b.set_visible(false);
            h.frame();
            CHECK(grid.columns() == 2);
            CHECK(near(c.bounds().x, a.bounds().right() + 8.0f));
        }

        // A wrapping text is measured at the width its tile gets.
        {
            harness h({800, 400});
            auto &body = h.screen->emplace_root<column>();
            auto &grid = body.emplace<tile_grid>(180.0f, 8.0f);
            auto &text = grid.emplace<label>(
                "A description long enough to need a second line in a narrow tile, and a third in a narrower one.");
            text.set_wrap(true);
            fixed_tile(grid, 20.0f);
            h.frame();
            const float wide = text.bounds().height;
            h.screen->set_size({200, 400});
            h.frame();
            CHECK(grid.columns() == 1);
            CHECK(text.bounds().height > wide);
        }

        // columns_for() as documented.
        tile_grid grid(100.0f, 10.0f);
        CHECK(grid.columns_for(0.0f, 3) == 1);
        CHECK(grid.columns_for(209.0f, 3) == 1);
        CHECK(grid.columns_for(210.0f, 3) == 2);
        CHECK(grid.columns_for(10000.0f, 3) == 3);
        CHECK(grid.columns_for(10000.0f, 0) == 1);
    }
} // namespace

int main()
{
    strip_of_tools();
    running_log();
    tiles();
    return volt::tests::result("ui workspace widgets");
}
