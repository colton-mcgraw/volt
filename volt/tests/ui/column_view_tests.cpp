#include "harness.hpp"

#include <ui/widgets/column_view.hpp>

#include <cmath>
#include <format>
#include <string>
#include <vector>

// The column view: rows virtualized like any row view, columns laid out by
// width and flex (and left out by drop_order where there is no room), the
// header asking for a sort, a divider dragged to size a column, the empty
// text, and the cell painter.

using namespace volt::tests::ui;

namespace
{
    std::vector<table_column> three_columns()
    {
        return {{"Name", 200.0f, 2.0f, align::start, true, 0},
                {"Value", 80.0f, 1.0f, align::start, true, 0},
                {"Library", 100.0f, 1.0f, align::start, false, 1}};
    }

    // Cell texts made on demand, counting how many were asked for.
    struct table_data
    {
        std::size_t rows = 0;
        std::size_t asked = 0;
        std::string text;

        column_view::cell_text source()
        {
            return [this](std::size_t row, std::size_t column) -> std::string_view {
                ++asked;
                text = std::format("r{}c{}", row, column);
                return text;
            };
        }
    };

    void virtualized()
    {
        harness h({600, 300});
        auto &view = h.screen->emplace_root<column_view>();
        view.set_columns(three_columns());
        table_data data;
        data.rows = 100000;
        view.set_rows(data.rows, data.source());
        h.frame();

        CHECK(view.row_count() == 100000);
        CHECK(view.top_margin() == view.header_height());
        // Only the visible rows are asked for: about (300 - header) / row
        // rows, three cells each.
        const auto [first, last] = view.visible_range();
        CHECK(first == 0);
        CHECK(last - first <= static_cast<std::size_t>(std::ceil(300.0f / view.row_height())));
        CHECK(view.painted_rows() == last - first);
        CHECK(data.asked <= 3 * (last - first));

        // The first row is below the header.
        CHECK(view.row_rect(0).y >= view.header_rect().bottom() - 0.5f);
        CHECK(view.row_at({50.0f, view.header_rect().y + 2.0f}) == -1);

        view.scroll_to(50000.0f * view.row_height());
        data.asked = 0;
        h.frame();
        CHECK(view.visible_range().first == 50000);
        CHECK(data.asked <= 3 * view.painted_rows());
    }

    void column_layout()
    {
        harness h({600, 300});
        auto &view = h.screen->emplace_root<column_view>();
        view.set_columns(three_columns());
        view.set_rows(10, table_data{}.source());
        h.frame();

        // Room to spare: every column at least its width, the rest by flex,
        // the columns side by side across the rows' width.
        const float w0 = view.column_width(0);
        const float w1 = view.column_width(1);
        const float w2 = view.column_width(2);
        CHECK(w0 >= 200.0f);
        CHECK(w1 >= 80.0f);
        CHECK(w2 >= 100.0f);
        const rect row = view.row_rect(0);
        CHECK(std::abs(w0 + w1 + w2 - row.width) < 0.5f);
        CHECK(std::abs(view.column_x(0) - row.x) < 0.5f);
        CHECK(std::abs(view.column_x(1) - (row.x + w0)) < 0.5f);
        // The extra went 2:1:1.
        CHECK(std::abs((w0 - 200.0f) - 2.0f * (w1 - 80.0f)) < 1.0f);

        // Too narrow for all three: the droppable one goes first.
        h.screen->set_size({330, 300});
        h.frame();
        CHECK(view.column_width(2) == 0.0f);
        CHECK(view.column_width(0) > 0.0f);
        CHECK(view.column_width(1) > 0.0f);

        // Narrower still: the rest shrink alike, and still fill the width.
        h.screen->set_size({200, 300});
        h.frame();
        CHECK(view.column_width(2) == 0.0f);
        const float sum = view.column_width(0) + view.column_width(1);
        CHECK(std::abs(sum - view.row_rect(0).width) < 0.5f);
        CHECK(std::abs(view.column_width(0) / view.column_width(1) - 2.5f) < 0.05f);
    }

    void header_sorts()
    {
        harness h({600, 300});
        auto &view = h.screen->emplace_root<column_view>();
        view.set_columns(three_columns());
        view.set_rows(10, table_data{}.source());
        h.frame();

        std::vector<std::pair<int, bool>> asked;
        view.sort_requested.connect([&](int column, bool ascending) { asked.emplace_back(column, ascending); });

        const float y = view.header_rect().y + view.header_rect().height * 0.5f;
        const auto click_column = [&](std::size_t i) {
            h.click(static_cast<int>(view.column_x(i) + 20.0f), static_cast<int>(y));
        };

        click_column(1);
        CHECK(asked.size() == 1 && asked.back() == std::make_pair(1, true));
        // The view only shows what the owner says it sorted by.
        CHECK(view.sort_column() == -1);
        view.set_sort(1, true);
        click_column(1);
        CHECK(asked.size() == 2 && asked.back() == std::make_pair(1, false));
        click_column(0);
        CHECK(asked.size() == 3 && asked.back() == std::make_pair(0, true));

        // Not sortable: nothing asked.
        click_column(2);
        CHECK(asked.size() == 3);

        // A press on the header leaves the selection alone.
        view.set_selected(4);
        click_column(0);
        CHECK(view.selected() == 4);

        // Out-of-range sorts are none.
        view.set_sort(7, true);
        CHECK(view.sort_column() == -1);
    }

    void divider_sizes_a_column()
    {
        harness h({600, 300});
        auto &view = h.screen->emplace_root<column_view>();
        view.set_columns(three_columns());
        view.set_rows(10, table_data{}.source());
        h.frame();

        const float before = view.column_width(0);
        const int edge = static_cast<int>(view.column_x(0) + before);
        const int y = static_cast<int>(view.header_rect().y + view.header_rect().height * 0.5f);
        CHECK(view.cursor({static_cast<float>(edge), static_cast<float>(y)}) == cursor_shape::resize_horizontal);

        h.drag(edge, y, edge - 60, y);
        h.frame();
        CHECK(std::abs(view.column_width(0) - (before - 60.0f)) < 1.0f);
        // It keeps its width now; the others took up the difference.
        CHECK(view.columns()[0].flex == 0.0f);
        const float sum = view.column_width(0) + view.column_width(1) + view.column_width(2);
        CHECK(std::abs(sum - view.row_rect(0).width) < 0.5f);

        // Not below the minimum.
        h.drag(static_cast<int>(view.column_x(0) + view.column_width(0)), y, 0, y);
        h.frame();
        CHECK(view.column_width(0) >= 23.5f);
    }

    void keys_and_activation()
    {
        harness h({600, 300});
        auto &view = h.screen->emplace_root<column_view>();
        view.set_columns(three_columns());
        view.set_rows(50, table_data{}.source());
        h.frame();

        int activated = -1;
        int changed = -1;
        view.activated.connect([&](int row) { activated = row; });
        view.selection_changed.connect([&](int row) { changed = row; });

        // A click on a row selects it; keys move on; Enter activates.
        const rect row2 = view.row_rect(2);
        h.click(static_cast<int>(row2.x + 30.0f), static_cast<int>(row2.y + row2.height * 0.5f));
        CHECK(view.selected() == 2);
        CHECK(changed == 2);
        h.key(key::down);
        CHECK(view.selected() == 3);
        h.key(key::end);
        CHECK(view.selected() == 49);
        h.key(key::enter);
        CHECK(activated == 49);
    }

    void empty_text_and_cell_painter()
    {
        harness h({600, 300});
        auto &view = h.screen->emplace_root<column_view>();
        view.set_columns(three_columns());
        view.set_rows(0, {});
        h.frame();
        const std::size_t glyphs_without = count(h.list, shape_kind::glyph);

        view.set_empty_text("Nothing here yet");
        h.frame();
        CHECK(count(h.list, shape_kind::glyph) > glyphs_without);

        // A cell painter takes the cells it wants.
        std::size_t painted = 0;
        view.set_cell_painter([&](painter &, std::size_t, std::size_t column, const rect &) {
            if (column != 1)
                return false;
            ++painted;
            return true;
        });
        table_data data;
        view.set_rows(5, data.source());
        h.frame();
        CHECK(painted == 5);
        CHECK(data.asked == 10); // the other two columns of five rows
    }
} // namespace

int main()
{
    virtualized();
    column_layout();
    header_sorts();
    divider_sizes_a_column();
    keys_and_activation();
    empty_text_and_cell_painter();
    return volt::tests::result("column view");
}
