#include "harness.hpp"

#include <ui/widgets/toggle_list.hpp>

#include <string>
#include <vector>

// The toggle list: switches flipped by pointer and keyboard without
// selecting, headings and notes that the selection passes over, Delete
// asked for, and a list that only shows what its owner says.

using namespace volt::tests::ui;

namespace
{
    toggle_row make(std::string text, std::string detail = {}, bool header = false, bool switchable = true,
                    bool on = true, bool selectable = true)
    {
        toggle_row r;
        r.text = std::move(text);
        r.detail = std::move(detail);
        r.header = header;
        r.switchable = switchable;
        r.on = on;
        r.selectable = selectable;
        return r;
    }

    // Board, two parts, an empty group with a note.
    std::vector<toggle_row> sample()
    {
        return {make("Board", {}, true),       make("Body", "80 x 50"),
                make("Parts", {}, true, false), make("U1", "SOIC-8"),
                make("R1", "1206", false, true, false), make("Dropped", {}, true),
                make("Drop a model here", {}, false, false, true, false)};
    }

    volt::math::vec2i pixel(const rect &r)
    {
        return {static_cast<int>(r.center().x()), static_cast<int>(r.center().y())};
    }

    void switches()
    {
        harness h({300, 300});
        auto &list = h.screen->emplace_root<toggle_list>(sample());
        h.screen->layout();
        h.frame();

        std::vector<std::pair<int, bool>> flips;
        list.toggled.connect([&](int row, bool on) { flips.emplace_back(row, on); });
        int changes = 0;
        list.selection_changed.connect([&](int) { ++changes; });

        // The eye flips, and asks to turn the row off; the row is not
        // selected, and the list shows what it showed until told otherwise.
        const volt::math::vec2i eye = pixel(list.switch_rect(3));
        h.click(eye.x(), eye.y());
        CHECK(flips.size() == 1 && flips[0] == std::pair<int, bool>{3, false});
        CHECK(list.selected() == -1 && changes == 0);
        CHECK(list.row(3).on);
        list.set_on(3, false);
        CHECK(!list.row(3).on);

        // An off switch asks to be turned on; a heading's switch is its
        // group's.
        h.click(pixel(list.switch_rect(4)).x(), pixel(list.switch_rect(4)).y());
        CHECK(flips.size() == 2 && flips[1] == std::pair<int, bool>{4, true});
        h.click(pixel(list.switch_rect(0)).x(), pixel(list.switch_rect(0)).y());
        CHECK(flips.size() == 3 && flips[2] == std::pair<int, bool>{0, false});

        // A heading without a switch, and a note, do nothing when pressed.
        h.click(pixel(list.switch_rect(2)).x(), pixel(list.switch_rect(2)).y());
        h.click(pixel(list.row_rect(6)).x(), pixel(list.row_rect(6)).y());
        CHECK(flips.size() == 3 && list.selected() == -1);

        // The text selects.
        h.click(pixel(list.row_rect(4)).x() + 40, pixel(list.row_rect(4)).y());
        CHECK(list.selected() == 4 && changes == 1);
        h.click(pixel(list.row_rect(2)).x() + 40, pixel(list.row_rect(2)).y());
        CHECK(list.selected() == 4 && changes == 1);

        // The switch's icon is painted for every switchable row.
        const draw_list &drawn = h.frame();
        CHECK(count(drawn, shape_kind::line) > 0);
        CHECK(list.painted_rows() == 7);
    }

    void keyboard()
    {
        harness h({300, 300});
        auto &list = h.screen->emplace_root<toggle_list>(sample());
        h.screen->layout();
        list.focus();

        std::vector<std::pair<int, bool>> flips;
        list.toggled.connect([&](int row, bool on) { flips.emplace_back(row, on); });
        std::vector<int> deletions;
        list.delete_requested.connect([&](int row) { deletions.push_back(row); });

        // Down from nothing lands on the first selectable row, past the
        // heading; on down past the next heading; never on the note.
        h.key(key::down);
        CHECK(list.selected() == 1);
        h.key(key::down);
        CHECK(list.selected() == 3);
        h.key(key::down);
        CHECK(list.selected() == 4);
        h.key(key::down);
        CHECK(list.selected() == 4);
        h.key(key::end);
        CHECK(list.selected() == 4);
        h.key(key::home);
        CHECK(list.selected() == 1);
        h.key(key::up);
        CHECK(list.selected() == 1);

        // Space flips the selected row's switch, Delete asks.
        h.key(key::down);
        h.key(key::space);
        CHECK(flips.size() == 1 && flips[0] == std::pair<int, bool>{3, false});
        h.key(key::del);
        CHECK(deletions == std::vector<int>{3});

        // New rows keep the selection where it was, off headings.
        std::vector<toggle_row> fewer = sample();
        fewer.erase(fewer.begin() + 1); // the body goes: row 3 is R1 now, 2 is U1
        list.set_rows(std::move(fewer));
        CHECK(list.selected() == 3 && list.row(3).text == "R1");
        std::vector<toggle_row> headings{make("A", {}, true), make("B", {}, true)};
        list.set_rows(std::move(headings));
        CHECK(list.selected() == -1);

        // Disabled, the switches do not flip.
        list.set_rows(sample());
        list.set_enabled(false);
        h.screen->layout();
        h.click(pixel(list.switch_rect(1)).x(), pixel(list.switch_rect(1)).y());
        CHECK(flips.size() == 1);
    }
} // namespace

int main()
{
    switches();
    keyboard();
    return volt::tests::result("ui toggle list");
}
