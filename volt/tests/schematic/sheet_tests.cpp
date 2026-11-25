// The sheet: grid snapping, points on wires, label kinds, ids and item
// references, and the sheet's change type through an undo stack (inserts,
// erases, a drag of parts, wires and labels together merging per record).

#include "../math/check.hpp"

#include <schematic/sheet.hpp>

#include <set>
#include <string>
#include <unordered_set>
#include <utility>

using namespace volt;
using namespace volt::schematic;

namespace
{
    void grid()
    {
        CHECK(snap_to_grid({0, 0}) == point{0, 0});
        CHECK(snap_to_grid({149, -149}) == point{100, -100});
        CHECK(snap_to_grid({150, -150}) == point{200, -200});
        CHECK(snap_to_grid({-49, 51}) == point{0, 100});
        CHECK(snap_to_grid({1234, 5678}, 50) == point{1250, 5700});
        CHECK(snap_to_grid({7, 8}, 0) == point{7, 8}); // no grid
        CHECK(units_per_grid == 100);
    }

    void wires()
    {
        const wire flat{{0, 0}, {300, 0}, {}};
        CHECK(on_wire(flat, {0, 0}) && on_wire(flat, {300, 0}) && on_wire(flat, {150, 0}));
        CHECK(!on_wire(flat, {301, 0}) && !on_wire(flat, {-1, 0}) && !on_wire(flat, {100, 1}));
        CHECK(inside_wire(flat, {100, 0}) && !inside_wire(flat, {0, 0}) && !inside_wire(flat, {300, 0}));

        const wire upright{{100, 200}, {100, -200}, {}};
        CHECK(on_wire(upright, {100, 0}) && inside_wire(upright, {100, -199}) && !on_wire(upright, {100, 201}));

        // Diagonal: exactly on the line only.
        const wire diagonal{{0, 0}, {300, 200}, {}};
        CHECK(on_wire(diagonal, {150, 100}) && !on_wire(diagonal, {150, 101}) && !on_wire(diagonal, {450, 300}));
        CHECK(inside_wire(diagonal, {150, 100}));

        // A wire of no length is its one point.
        const wire dot{{5, 5}, {5, 5}, {}};
        CHECK(on_wire(dot, {5, 5}) && !inside_wire(dot, {5, 5}) && !on_wire(dot, {5, 6}));

        // Large coordinates do not overflow.
        const wire huge{{-2'000'000'000, 0}, {2'000'000'000, 2'000'000'000}, {}};
        CHECK(on_wire(huge, {0, 1'000'000'000}) && !on_wire(huge, {0, 999'999'999}));
    }

    void label_kinds()
    {
        for (const label_kind kind : {label_kind::net, label_kind::power, label_kind::note})
        {
            label_kind read = label_kind::note;
            CHECK(parse_label_kind(to_string(kind), read) && read == kind);
        }
        label_kind untouched = label_kind::power;
        CHECK(!parse_label_kind("bus", untouched) && untouched == label_kind::power);
        CHECK(to_string(label_kind::net) == "net" && to_string(label_kind::power) == "power" &&
              to_string(label_kind::note) == "note");
    }

    void items()
    {
        sheet s;
        CHECK(s.max_id() == 0);
        s.parts.insert(4, part{"R", {0, 0}, 0, false, "R1", "1k", "resistor", {}, {}});
        s.wires.insert(9, wire{{0, 0}, {100, 0}, {}});
        s.labels.insert(2, label{label_kind::net, {100, 0}, 0, "OUT", {}});
        CHECK(s.max_id() == 9);

        CHECK(contains(s, {item_kind::part, 4}) && contains(s, {item_kind::wire, 9}) &&
              contains(s, {item_kind::label, 2}));
        CHECK(!contains(s, {item_kind::wire, 4}) && !contains(s, {item_kind::part, 9}));

        // Comparable, orderable, hashable.
        const item_ref a{item_kind::part, 7};
        const item_ref b{item_kind::wire, 1};
        const item_ref c{item_kind::part, 8};
        CHECK(a < b && a < c && c < b && a == item_ref{item_kind::part, 7} && a != c);
        const std::set<item_ref> ordered{b, c, a, a};
        CHECK(ordered.size() == 3 && *ordered.begin() == a);
        const std::unordered_set<item_ref> hashed{b, c, a, a};
        CHECK(hashed.size() == 3 && hashed.contains(c));
        CHECK(std::hash<item_ref>{}(a) != std::hash<item_ref>{}(item_ref{item_kind::wire, 7}));

        // Equality is the records, in order, and the extra fields.
        sheet t = s;
        CHECK(t == s);
        t.extra.set("title", json::value("x"));
        CHECK(!(t == s));
    }

    void changes()
    {
        sheet s;
        sheet_history undo;

        {
            auto t = undo.transact(s, "Draw");
            t.record(sheet_change{parts_change{*s.parts.insert(1, part{"R", {0, 0}, 0, false, "R1", "", "", {}, {}})}});
            t.record(sheet_change{wires_change{*s.wires.insert(2, wire{{100, 0}, {300, 0}, {}})}});
            t.record(sheet_change{labels_change{*s.labels.insert(3, label{label_kind::net, {300, 0}, 0, "A", {}})}});
            t.record(sheet_change{parts_change{*s.parts.insert(4, part{"C", {500, 0}, 0, false, "C1", "", "", {}, {}})}});
            CHECK(t.commit());
        }
        const sheet drawn = s;
        CHECK(undo.steps().size() == 1 && undo.steps()[0].changes.size() == 4);

        // A drag of three kinds of record at once: every move is a step,
        // merged by key into the first, one change per record.
        for (int step = 1; step <= 100; ++step)
        {
            undo.begin("Drag");
            part p = *s.parts.find(1);
            p.position = {step, 0};
            undo.record(sheet_change{parts_change{*s.parts.replace(1, p)}});
            wire w = *s.wires.find(2);
            w.a = {100 + step, 0};
            w.b = {300 + step, 0};
            undo.record(sheet_change{wires_change{*s.wires.replace(2, w)}});
            label l = *s.labels.find(3);
            l.position = {300 + step, 0};
            undo.record(sheet_change{labels_change{*s.labels.replace(3, l)}});
            undo.commit("drag");
        }
        CHECK(undo.steps().size() == 2 && undo.steps()[1].changes.size() == 3);
        const sheet dragged = s;

        // Deleting from the middle and undoing puts it back where it was.
        undo.record("Delete", sheet_change{parts_change{*s.parts.erase(1)}});
        CHECK(s.parts.size() == 1 && s.parts[0].id == 4);
        CHECK(undo.undo(s) && s == dragged && s.parts[0].id == 1);
        CHECK(undo.undo(s) && s == drawn);
        CHECK(undo.undo(s) && s == sheet{});
        CHECK(undo.redo(s) && undo.redo(s) && s == dragged);
    }

} // namespace

int main()
{
    grid();
    wires();
    label_kinds();
    items();
    changes();
    return volt::tests::result("schematic sheet");
}
