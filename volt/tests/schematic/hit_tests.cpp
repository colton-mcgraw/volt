// Hit testing: parts, wires and labels under the pointer in every
// orientation, at the edges of the tolerance, in priority order when they
// overlap; rectangle selection wholly inside or crossing; wire ends for
// dragging; what a wire being drawn snaps to; and the boxes of items and
// selections, with the pivot a selection turns about.

#include "../math/check.hpp"

#include <schematic/hit.hpp>
#include <schematic/stroke_font.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace volt;
using namespace volt::schematic;

namespace
{
    math::vec2f at(float x, float y)
    {
        return math::vec2f{x, y};
    }

    math::vec2f at(point p)
    {
        return math::vec2f{static_cast<float>(p.x()), static_cast<float>(p.y())};
    }

    bool hits(const sheet &s, math::vec2f where, float tolerance, item_ref expected)
    {
        const auto found = hit_test(s, where, tolerance);
        return found && *found == expected;
    }

    void parts()
    {
        // A resistor (body box 200 by 50) in every turn, mirrored or not.
        for (std::uint8_t r = 0; r < 4; ++r)
        {
            for (const bool mirrored : {false, true})
            {
                sheet s;
                s.parts.insert(5, part{"R", {1000, 1000}, r, mirrored, "R1", "1k", "", {}, {}});
                const item_ref item{item_kind::part, 5};
                CHECK(hits(s, at(1000, 1000), 10.0f, item));
                if (r % 2 == 0)
                {
                    CHECK(hits(s, at(1080, 1030), 10.0f, item));   // 5 past the body, within 10
                    CHECK(!hit_test(s, at(1080, 1040), 10.0f));    // 15 past it
                    CHECK(hits(s, at(890, 1000), 10.0f, item));    // past the pin by 10
                    CHECK(!hit_test(s, at(889, 1000), 10.0f));
                }
                else
                {
                    CHECK(hits(s, at(1030, 1080), 10.0f, item));
                    CHECK(!hit_test(s, at(1040, 1080), 10.0f));
                    CHECK(hits(s, at(1000, 890), 10.0f, item));
                    CHECK(!hit_test(s, at(1000, 889), 10.0f));
                }
                // The reference and value are not the part.
                CHECK(!hit_test(s, at(transform(s.parts.entries()[0].record, point{0, 75})), 1.0f));
            }
        }

        // A lopsided symbol: a point of its body is hit wherever the part
        // turns; a point outside its box is not.
        for (std::uint8_t r = 0; r < 4; ++r)
        {
            for (const bool mirrored : {false, true})
            {
                sheet s;
                const part q{"Q_NPN", {-300, 700}, r, mirrored, "Q1", "", "", {}, {}};
                s.parts.insert(1, q);
                CHECK(hits(s, at(transform(q, point{140, 150})), 5.0f, item_ref{item_kind::part, 1}));
                CHECK(hits(s, at(transform(q, point{-190, -190})), 5.0f, item_ref{item_kind::part, 1}));
                CHECK(!hit_test(s, at(transform(q, point{140, 260})), 5.0f));
                CHECK(!hit_test(s, at(transform(q, point{-260, 0})), 5.0f));
            }
        }
    }

    void wires()
    {
        sheet s;
        s.wires.insert(1, wire{{0, 0}, {1000, 0}, {}});
        s.wires.insert(2, wire{{0, 2000}, {1000, 3000}, {}});
        const item_ref horizontal{item_kind::wire, 1};
        const item_ref diagonal{item_kind::wire, 2};

        CHECK(hits(s, at(500, 10), 10.0f, horizontal));
        CHECK(!hit_test(s, at(500, 10.5f), 10.0f));
        CHECK(hits(s, at(500, -9), 10.0f, horizontal));
        CHECK(hits(s, at(1007, 7), 10.0f, horizontal)); // round past the end
        CHECK(!hit_test(s, at(1008, 8), 10.0f));
        CHECK(hits(s, at(-10, 0), 10.0f, horizontal));
        CHECK(!hit_test(s, at(-10.5f, 0), 10.0f));

        CHECK(hits(s, at(500, 2510), 8.0f, diagonal)); // 7.07 from it
        CHECK(!hit_test(s, at(500, 2510), 7.0f));

        // The nearest of two first, both listed.
        s.wires.insert(3, wire{{0, 20}, {1000, 20}, {}});
        const auto all = hit_test_all(s, at(500, 12), 15.0f);
        CHECK(all == (std::vector<item_ref>{{item_kind::wire, 3}, {item_kind::wire, 1}}));
        // Equally near: the later.
        CHECK(hits(s, at(500, 10), 10.0f, item_ref{item_kind::wire, 3}));
    }

    void labels()
    {
        const draw_style style;
        const float length = text_width("NET", style.label_height) * 100.0f;
        const float along = 10.0f + 0.5f * length;      // the middle of the text
        const float across = 16.0f + 0.5f * 100.0f * style.label_height; // half its height, off the wire
        const math::vec2f middles[] = {at(along, across), at(-across, along), at(-along, across), at(-across, -along)};
        const math::vec2f wrong_side[] = {at(along, -across), at(across, along), at(-along, -across),
                                          at(across, -along)};
        for (std::uint8_t r = 0; r < 4; ++r)
        {
            sheet s;
            s.labels.insert(1, label{label_kind::net, {0, 0}, r, "NET", {}});
            CHECK(hits(s, middles[r], 1.0f, item_ref{item_kind::label, 1}));
            CHECK(!hit_test(s, wrong_side[r], 1.0f));
        }

        // Power ports, by their glyphs and text.
        for (std::uint8_t r = 0; r < 4; ++r)
        {
            sheet s;
            s.labels.insert(1, label{label_kind::power, {500, 500}, r, "GND", {}});
            s.labels.insert(2, label{label_kind::power, {-500, 500}, r, "+12V", {}});
            CHECK(hits(s, at(orient({0, -75}, r, false) + point{500, 500}), 1.0f, item_ref{item_kind::label, 1}));
            CHECK(!hit_test(s, at(orient({0, 75}, r, false) + point{500, 500}), 1.0f));
            CHECK(hits(s, at(orient({0, 35}, r, false) + point{-500, 500}), 1.0f, item_ref{item_kind::label, 2}));
            CHECK(hits(s, at(orient({0, 100}, r, false) + point{-500, 500}), 1.0f, item_ref{item_kind::label, 2}));
        }

        // A note, by its text.
        sheet s;
        s.labels.insert(1, label{label_kind::note, {0, 0}, 0, "hello", {}});
        CHECK(hits(s, at(50, 20), 1.0f, item_ref{item_kind::label, 1}));
        CHECK(!hit_test(s, at(50, 80), 1.0f));
    }

    void priority()
    {
        // A label on a wire on a part's pin: label, wire, part.
        sheet s;
        s.parts.insert(1, part{"R", {0, 0}, 0, false, "R1", "1k", "", {}, {}});
        s.wires.insert(2, wire{{100, 0}, {500, 0}, {}});
        s.labels.insert(3, label{label_kind::net, {100, 0}, 0, "A", {}});
        CHECK(hit_test_all(s, at(110, 5), 10.0f) ==
              (std::vector<item_ref>{{item_kind::label, 3}, {item_kind::wire, 2}, {item_kind::part, 1}}));
        CHECK(hits(s, at(110, 5), 10.0f, item_ref{item_kind::label, 3}));
        CHECK(hits(s, at(300, -5), 10.0f, item_ref{item_kind::wire, 2}));
        CHECK(hits(s, at(0, 0), 10.0f, item_ref{item_kind::part, 1}));
        CHECK(!hit_test(s, at(0, -500), 10.0f) && hit_test_all(s, at(0, -500), 10.0f).empty());

        // The smaller part over the bigger; of two the same, the later.
        sheet t;
        t.parts.insert(1, part{"R", {0, 0}, 0, false, "R1", "", "", {}, {}});
        t.parts.insert(2, part{"MCU", {0, 0}, 0, false, "U1", "", "", {}, {}});
        t.parts.insert(3, part{"R", {0, 0}, 0, false, "R2", "", "", {}, {}});
        CHECK(hit_test_all(t, at(0, 0), 1.0f) ==
              (std::vector<item_ref>{{item_kind::part, 3}, {item_kind::part, 1}, {item_kind::part, 2}}));
        CHECK(hits(t, at(0, 300), 1.0f, item_ref{item_kind::part, 2}));
    }

    void rectangles()
    {
        sheet s;
        s.parts.insert(1, part{"R", {0, 0}, 0, false, "R1", "1k", "", {}, {}}); // body -100..100, -25..25
        s.wires.insert(2, wire{{0, 300}, {300, 300}, {}});
        s.wires.insert(3, wire{{-500, 600}, {500, 600}, {}});
        s.wires.insert(4, wire{{0, 1500}, {500, 1000}, {}}); // x + y = 1500
        s.labels.insert(5, label{label_kind::net, {0, -300}, 0, "X", {}});

        using v = std::vector<item_ref>;
        // Wholly inside, or touched.
        CHECK(items_in_rect(s, {-200, -400}, {400, 400}, false) ==
              (v{{item_kind::part, 1}, {item_kind::wire, 2}, {item_kind::label, 5}}));
        CHECK(items_in_rect(s, {400, 400}, {-200, -400}, false) == items_in_rect(s, {-200, -400}, {400, 400}, false));
        CHECK(items_in_rect(s, {50, 0}, {200, 350}, false).empty());
        CHECK(items_in_rect(s, {50, 0}, {200, 350}, true) == (v{{item_kind::part, 1}, {item_kind::wire, 2}}));
        // A wire passing through, both ends outside.
        CHECK(items_in_rect(s, {-100, 500}, {100, 700}, true) == (v{{item_kind::wire, 3}}));
        CHECK(items_in_rect(s, {-100, 500}, {100, 700}, false).empty());
        // A diagonal whose box overlaps the rectangle but which misses it.
        CHECK(items_in_rect(s, {0, 1000}, {200, 1200}, true).empty());
        CHECK(items_in_rect(s, {0, 1000}, {300, 1200}, true) == (v{{item_kind::wire, 4}}));
        // Edges count: a rectangle that ends on a wire touches it.
        CHECK(items_in_rect(s, {0, 0}, {300, 300}, true).size() == 2);
        CHECK(items_in_rect(s, {0, 0}, {300, 300}, false) == (v{{item_kind::wire, 2}}));
        // Everything, in order.
        CHECK(items_in_rect(s, {-10000, -10000}, {10000, 10000}, false) ==
              (v{{item_kind::part, 1},
                 {item_kind::wire, 2},
                 {item_kind::wire, 3},
                 {item_kind::wire, 4},
                 {item_kind::label, 5}}));
    }

    void wire_ends()
    {
        sheet s;
        s.wires.insert(1, wire{{0, 0}, {300, 0}, {}});
        s.wires.insert(2, wire{{300, 0}, {300, 300}, {}});

        CHECK(hit_wire_end(s, at(295, 3), 10.0f) == wire_end_hit{2, false}); // shared: the later
        CHECK(hit_wire_end(s, at(5, 0), 10.0f) == wire_end_hit{1, false});
        CHECK(hit_wire_end(s, at(300, 290), 10.0f) == wire_end_hit{2, true});
        CHECK(!hit_wire_end(s, at(300, 289), 10.0f));
        CHECK(!hit_wire_end(s, at(150, 0), 10.0f));
        CHECK(hit_wire_end(s, at(290, 0), 10.0f) == wire_end_hit{2, false});
        CHECK(wire_ends_at(s, {300, 0}) == (std::vector<wire_end_hit>{{1, true}, {2, false}}));
        CHECK(wire_ends_at(s, {150, 0}).empty());
    }

    void snapping()
    {
        sheet s;
        s.parts.insert(1, part{"R", {0, 0}, 0, false, "R1", "1k", "", {}, {}}); // pins (-100, 0), (100, 0)
        s.wires.insert(2, wire{{100, 0}, {500, 0}, {}});
        s.wires.insert(3, wire{{0, 300}, {400, 700}, {}});  // diagonal, on the grid
        s.wires.insert(4, wire{{50, 1000}, {350, 1300}, {}}); // diagonal, off the grid
        s.labels.insert(5, label{label_kind::net, {500, 0}, 0, "OUT", {}});
        s.labels.insert(6, label{label_kind::power, {1000, 1000}, 0, "+5V", {}});
        s.labels.insert(7, label{label_kind::note, {2000, 2000}, 0, "no", {}});

        // A pin, and a pin over the wire end on it.
        {
            const auto c = nearest_connection(s, at(-95, 4), 50.0f);
            CHECK(c && c->kind == connection_kind::pin && c->at == point{-100, 0} &&
                  c->item == item_ref{item_kind::part, 1} && c->pin && c->pin->number == "1");
            const auto d = nearest_connection(s, at(102, 3), 50.0f);
            CHECK(d && d->kind == connection_kind::pin && d->at == point{100, 0} && d->pin->number == "2");
        }
        // A wire end over the label on it.
        {
            const auto c = nearest_connection(s, at(510, 10), 50.0f);
            CHECK(c && c->kind == connection_kind::wire_end && c->at == point{500, 0} &&
                  c->item == item_ref{item_kind::wire, 2} && c->end_b);
        }
        // A label alone; not a note.
        {
            const auto c = nearest_connection(s, at(1010, 990), 50.0f);
            CHECK(c && c->kind == connection_kind::label && c->item == item_ref{item_kind::label, 6} &&
                  c->at == point{1000, 1000});
            CHECK(!nearest_connection(s, at(2000, 2000), 50.0f));
        }
        // Inside a wire: the grid point.
        {
            const auto c = nearest_connection(s, at(290, 20), 50.0f);
            CHECK(c && c->kind == connection_kind::wire_inside && c->at == point{300, 0} &&
                  c->item == item_ref{item_kind::wire, 2});
            const auto d = nearest_connection(s, at(140, 452), 80.0f);
            CHECK(d && d->kind == connection_kind::wire_inside && d->at == point{100, 400} &&
                  d->item == item_ref{item_kind::wire, 3});
            // No grid point inside near enough: nothing (the wire is on the
            // grid, and its ends are far).
            CHECK(!nearest_connection(s, at(148, 452), 50.0f));
        }
        // Off the grid: the nearest integer point inside it.
        {
            const auto c = nearest_connection(s, at(152, 1099), 50.0f);
            CHECK(c && c->kind == connection_kind::wire_inside && c->item == item_ref{item_kind::wire, 4});
            if (c)
            {
                const wire &w = *s.wires.find(4);
                const float dx = static_cast<float>(c->at.x()) - 152.0f;
                const float dy = static_cast<float>(c->at.y()) - 1099.0f;
                CHECK(inside_wire(w, c->at) && std::sqrt(dx * dx + dy * dy) <= 3.0f);
            }
        }
        // The end, not the inside, when the end is nearest.
        {
            const auto c = nearest_connection(s, at(497, 0), 50.0f);
            CHECK(c && c->kind == connection_kind::wire_end);
        }
        // Tolerance.
        CHECK(!nearest_connection(s, at(-100, 60), 50.0f));
        CHECK(nearest_connection(s, at(-100, 50), 50.0f).has_value());
        CHECK(!nearest_connection(sheet{}, at(0, 0), 1000.0f));

        // Items excluded offer nothing: their ends, insides, pins, points.
        {
            const std::vector<item_ref> wire_2{{item_kind::wire, 2}};
            const auto c = nearest_connection(s, at(510, 10), 50.0f, wire_2);
            CHECK(c && c->kind == connection_kind::label && c->item == item_ref{item_kind::label, 5});
            CHECK(!nearest_connection(s, at(290, 20), 50.0f, wire_2));
            const std::vector<item_ref> both{{item_kind::wire, 2}, {item_kind::label, 5}};
            CHECK(!nearest_connection(s, at(510, 10), 50.0f, both));
            const std::vector<item_ref> part_1{{item_kind::part, 1}};
            const auto d = nearest_connection(s, at(102, 3), 50.0f, part_1);
            CHECK(d && d->kind == connection_kind::wire_end && d->at == point{100, 0});
            CHECK(!nearest_connection(s, at(-95, 4), 50.0f, part_1));
            // Not on the sheet: no matter.
            const std::vector<item_ref> ghost{{item_kind::wire, 99}};
            CHECK(nearest_connection(s, at(-95, 4), 50.0f, ghost)->kind == connection_kind::pin);
        }
    }

    void boxes()
    {
        sheet s;
        s.parts.insert(1, part{"R", {150, 0}, 0, false, "R1", "1k", "", {}, {}});
        s.wires.insert(2, wire{{300, 500}, {-200, 400}, {}});
        s.labels.insert(3, label{label_kind::net, {0, -500}, 0, "N", {}});

        CHECK(item_bounds(s, {item_kind::part, 1}) == rect{{50, -25}, {250, 25}});
        CHECK(item_bounds(s, {item_kind::wire, 2}) == rect{{-200, 400}, {300, 500}});
        const auto l = item_bounds(s, {item_kind::label, 3});
        CHECK(l && l->min.x() <= 0 && l->min.y() <= -500 && l->max.y() > -450 && l->max.x() > 0);
        CHECK(!item_bounds(s, {item_kind::part, 2}) && !item_bounds(s, {item_kind::wire, 1}));

        const std::vector<item_ref> one{{item_kind::part, 1}};
        CHECK(selection_bounds(s, one) == rect{{50, -25}, {250, 25}});
        // The centre (150, 0) snaps away from zero, onto the grid.
        CHECK(rotation_pivot(s, one) == point{200, 0});
        const std::vector<item_ref> two{{item_kind::part, 1}, {item_kind::wire, 2}, {item_kind::label, 99}};
        CHECK(selection_bounds(s, two) == rect{{-200, -25}, {300, 500}});
        CHECK(rotation_pivot(s, two) == point{100, 200});
        CHECK(!selection_bounds(s, {}) && !rotation_pivot(s, {}));
        const std::vector<item_ref> missing{{item_kind::label, 99}};
        CHECK(!selection_bounds(s, missing));
    }

} // namespace

int main()
{
    parts();
    wires();
    labels();
    priority();
    rectangles();
    wire_ends();
    snapping();
    boxes();
    return volt::tests::result("schematic hit");
}
