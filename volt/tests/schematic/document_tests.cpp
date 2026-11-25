// The edited document: every operation and its undo and redo (which must
// give back the sheet exactly, table order included), ids, references,
// drags merged into one step and one change per record, transactions,
// rotating and mirroring as true rotations and mirror images, paste, tidying
// wires, the saved state, notification, and a long random run of edits,
// undos and redos checked against snapshots.

#include "../math/check.hpp"

#include <schematic/document.hpp>
#include <schematic/symbol.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace volt;
using namespace volt::schematic;

namespace
{
    part make_part(std::string symbol, point at, std::string reference = {}, std::uint8_t rotation = 0,
                   bool mirrored = false)
    {
        part p;
        p.symbol = std::move(symbol);
        p.position = at;
        p.rotation = rotation;
        p.mirrored = mirrored;
        p.reference = std::move(reference);
        return p;
    }

    label make_label(label_kind kind, point at, std::string text, std::uint8_t rotation = 0)
    {
        label l;
        l.kind = kind;
        l.position = at;
        l.rotation = rotation;
        l.text = std::move(text);
        return l;
    }

    item_ref part_ref(item_id id) { return {item_kind::part, id}; }
    item_ref wire_ref(item_id id) { return {item_kind::wire, id}; }
    item_ref label_ref(item_id id) { return {item_kind::label, id}; }

    // The ids of a table, in order.
    template <typename Table>
    std::vector<item_id> ids_of(const Table &t)
    {
        std::vector<item_id> ids;
        for (const auto &e : t)
            ids.push_back(e.id);
        return ids;
    }

    // A small sheet to start from: R1 and C1 with a wire between them and
    // a label on it.
    sheet small_sheet()
    {
        sheet s;
        s.parts.insert(1, make_part("R", {0, 0}, "R1"));
        s.parts.insert(2, make_part("C", {400, 0}, "C1"));
        s.wires.insert(3, wire{{100, 0}, {300, 0}, {}});
        s.labels.insert(4, make_label(label_kind::net, {200, 0}, "MID"));
        return s;
    }

    void construction()
    {
        const document empty;
        CHECK(empty.sheet() == sheet{} && empty.next_id() == 1 && !empty.modified());
        CHECK(!empty.can_undo() && !empty.can_redo() && empty.undo_label().empty());

        const document d(small_sheet());
        CHECK(d.sheet() == small_sheet() && d.next_id() == 5 && !d.modified());
        CHECK(d.find_part(1) && d.find_part(1)->reference == "R1" && !d.find_part(3));
        CHECK(d.find_wire(3) && d.find_label(4) && !d.find_label(1));
        CHECK(d.contains(part_ref(2)) && !d.contains(wire_ref(2)));
    }

    void adding()
    {
        document d;

        // A part: numbered from its symbol, its device from the symbol.
        const item_id r1 = d.add_part(make_part("R", {0, 0}));
        CHECK(r1 == 1 && d.find_part(r1)->reference == "R1" && d.find_part(r1)->device == "resistor");
        CHECK(d.undo_label() == "Place R1" && d.modified());
        const item_id r2 = d.add_part(make_part("R", {400, 0}, "R?"));
        CHECK(d.find_part(r2)->reference == "R2");
        const item_id r7 = d.add_part(make_part("R", {800, 0}, "R7"));
        CHECK(d.find_part(r7)->reference == "R7");
        CHECK(d.next_reference("R") == "R3");
        CHECK(d.find_part(d.add_part(make_part("C", {0, 400})))->reference == "C1");
        CHECK(d.find_part(d.add_part(make_part("Some_IC", {0, 800})))->reference == "U1");
        CHECK(d.find_part(d.add_part(make_part("Conn_01x04", {0, 1200})))->reference == "J1");
        part kept_device = make_part("R", {0, 1600});
        kept_device.device = "thermistor";
        CHECK(d.find_part(d.add_part(kept_device))->device == "thermistor");
        CHECK(d.history().steps().size() == 7);

        // A wire; one of no length is refused, and is no step.
        const std::size_t steps = d.history().steps().size();
        CHECK(d.add_wire({0, 0}, {0, 0}) == 0 && d.history().steps().size() == steps);
        const item_id w = d.add_wire({100, 0}, {300, 0});
        CHECK(w != 0 && d.undo_label() == "Draw wire" && d.find_wire(w)->b == point{300, 0});

        // Several wires as one step.
        const std::vector<std::pair<point, point>> segments{
            {{0, 0}, {0, 100}}, {{0, 100}, {0, 100}}, {{0, 100}, {100, 100}}, {{100, 100}, {100, 300}}};
        const std::vector<item_id> made = d.add_wires(segments);
        CHECK(made.size() == 4 && made[0] != 0 && made[1] == 0 && made[2] != 0 && made[3] != 0);
        CHECK(d.undo_label() == "Draw 3 wires" && d.history().steps().size() == steps + 2);
        const std::vector<point> path{{1000, 0}, {1000, 200}, {1200, 200}};
        const std::vector<item_id> path_ids = d.add_path(path);
        CHECK(path_ids.size() == 2 && d.find_wire(path_ids[1])->a == point{1000, 200});
        CHECK(d.add_wires(std::vector<std::pair<point, point>>{{{5, 5}, {5, 5}}}) == std::vector<item_id>{0});
        CHECK(d.add_path(std::vector<point>{{7, 7}}).empty());

        // Labels.
        const item_id gnd = d.add_label(make_label(label_kind::power, {0, -200}, "GND"));
        CHECK(d.undo_label() == "Place GND" && d.find_label(gnd)->text == "GND");
        d.add_label(make_label(label_kind::net, {0, 100}, "IN"));
        CHECK(d.undo_label() == "Add label");

        // Ids are unique across the tables and never reused, even after an
        // undo; redo brings the item back under its own id.
        std::vector<item_id> all = ids_of(d.sheet().parts);
        for (const item_id id : ids_of(d.sheet().wires))
            all.push_back(id);
        for (const item_id id : ids_of(d.sheet().labels))
            all.push_back(id);
        std::ranges::sort(all);
        CHECK(std::ranges::adjacent_find(all) == all.end() && all.front() != 0);

        const sheet before = d.sheet();
        const item_id last = d.add_part(make_part("L", {2000, 0}));
        const sheet with = d.sheet();
        CHECK(d.undo() && d.sheet() == before);
        CHECK(d.redo() && d.sheet() == with && d.find_part(last) != nullptr);
        CHECK(d.undo());
        const item_id again = d.add_part(make_part("L", {2000, 0}));
        CHECK(again != last && again > last && !d.can_redo());
    }

    void references()
    {
        document d;
        CHECK(d.next_reference("R") == "R1");
        for (const char *reference : {"R1", "R3", "R4", "C2", "RV1", "R03x"})
            d.add_part(make_part("R", {0, 0}, reference));
        CHECK(d.next_reference("R") == "R2" && d.next_reference("C") == "C1" && d.next_reference("RV") == "RV2");
        d.add_part(make_part("R", {0, 0}));
        CHECK(d.next_reference("R") == "R5");
        CHECK(d.next_reference("") == "1");

        CHECK(reference_prefix("R12") == "R" && reference_prefix("R?") == "R" && reference_prefix("U") == "U");
        CHECK(reference_prefix("12") == "" && reference_prefix("#PWR03") == "#PWR" && reference_prefix("") == "");
        CHECK(is_unnumbered_reference("") && is_unnumbered_reference("R?") && !is_unnumbered_reference("R1"));
    }

    void erasing()
    {
        document d(small_sheet());
        const sheet start = d.sheet();

        // From the middle of a table: undo puts it back in its place.
        CHECK(d.erase(std::vector{part_ref(1)}) == 1 && d.undo_label() == "Delete R1");
        CHECK(ids_of(d.sheet().parts) == std::vector<item_id>{2});
        CHECK(d.undo() && d.sheet() == start && ids_of(d.sheet().parts) == (std::vector<item_id>{1, 2}));

        // Several kinds at once, one step; unknown and repeated ones ignored.
        const std::vector<item_ref> items{wire_ref(3), label_ref(4), part_ref(2), part_ref(99), wire_ref(3)};
        CHECK(d.erase(items) == 3 && d.undo_label() == "Delete 3 items");
        CHECK(d.sheet().wires.empty() && d.sheet().labels.empty() && d.sheet().parts.size() == 1);
        CHECK(d.history().steps().size() == 1);
        CHECK(d.undo() && d.sheet() == start);
        CHECK(d.redo() && d.sheet().parts.size() == 1);
        CHECK(d.undo());

        // Nothing to erase: no step.
        CHECK(d.erase(std::vector{part_ref(42)}) == 0 && d.erase({}) == 0);
        CHECK(d.can_redo() && d.sheet() == start);
    }

    void moving()
    {
        document d(small_sheet());
        const sheet start = d.sheet();

        const std::vector<item_ref> all{part_ref(1), wire_ref(3), label_ref(4), part_ref(1)};
        CHECK(!d.move(all, {0, 0}) && !d.can_undo());
        CHECK(d.move(all, {100, -200}) && d.undo_label() == "Move 3 items");
        CHECK(d.find_part(1)->position == point{100, -200}); // once, though named twice
        CHECK(d.find_wire(3)->a == point{200, -200} && d.find_wire(3)->b == point{400, -200});
        CHECK(d.find_label(4)->position == point{300, -200});
        CHECK(d.find_part(2)->position == point{400, 0});
        CHECK(d.undo() && d.sheet() == start);
        CHECK(d.move(std::vector{part_ref(2)}, {0, 100}) && d.undo_label() == "Move C1");
        CHECK(d.move(std::vector{wire_ref(3)}, {0, 100}) && d.undo_label() == "Move wire");
        CHECK(d.move(std::vector{label_ref(4)}, {0, 100}) && d.undo_label() == "Move MID");
        CHECK(!d.move(std::vector{part_ref(77)}, {0, 100}));

        // A wire's end.
        document e(small_sheet());
        CHECK(e.move_wire_end(3, true, {300, 300}) && e.find_wire(3)->b == point{300, 300} &&
              e.find_wire(3)->a == point{100, 0});
        CHECK(e.move_wire_end(3, false, {0, 0}) && e.find_wire(3)->a == point{0, 0});
        CHECK(!e.move_wire_end(3, false, {0, 0}) && !e.move_wire_end(99, false, {0, 0}));
        CHECK(e.undo_label() == "Move wire end" && e.history().steps().size() == 2);
        CHECK(e.undo() && e.undo() && e.sheet() == small_sheet());
    }

    void dragging()
    {
        document d(small_sheet());
        const sheet start = d.sheet();
        const std::vector<item_ref> selection{part_ref(1), wire_ref(3), label_ref(4)};

        // 200 mouse moves: one step, one change per record.
        for (int i = 0; i < 200; ++i)
            CHECK(d.move(selection, {i % 2 == 0 ? 10 : -5, 3}, "drag:1"));
        CHECK(d.history().steps().size() == 1);
        CHECK(d.history().steps()[0].changes.size() == 3);
        const point moved{100 * 10 - 100 * 5, 600};
        CHECK(d.find_part(1)->position == moved && d.find_label(4)->position == point{200, 0} + moved);
        const sheet dragged = d.sheet();
        CHECK(d.undo() && d.sheet() == start && d.redo() && d.sheet() == dragged);

        // After an undo or a seal, the same key starts a new step.
        d.move(selection, {100, 0}, "drag:1");
        CHECK(d.history().steps().size() == 2);
        d.seal();
        d.move(selection, {100, 0}, "drag:1");
        CHECK(d.history().steps().size() == 3);
        d.move(selection, {100, 0}, "drag:2"); // another key
        CHECK(d.history().steps().size() == 4);

        // A drag that comes back to where it started leaves no step.
        document e(small_sheet());
        e.add_wire({0, 500}, {500, 500});
        const sheet before_drag = e.sheet();
        for (int i = 0; i < 50; ++i)
            e.move(selection, {7, -3}, "drag:9");
        CHECK(e.history().steps().size() == 2);
        for (int i = 0; i < 50; ++i)
            e.move(selection, {-7, 3}, "drag:9");
        CHECK(e.history().steps().size() == 1 && e.sheet() == before_drag && e.undo_label() == "Draw wire");
        // And it is not merged into by the next drag: the step below is sealed.
        e.move(std::vector{wire_ref(5)}, {0, 100}, "drag:9");
        CHECK(e.history().steps().size() == 2);

        // Back to the saved state: unmodified.
        document f(small_sheet());
        f.move(selection, {100, 0}, "drag:3");
        CHECK(f.modified());
        f.move(selection, {-100, 0}, "drag:3");
        CHECK(!f.modified() && !f.can_undo());

        // A drag of a wire's end merges the same way.
        document g(small_sheet());
        for (int x = 0; x < 300; x += 10)
            g.move_wire_end(3, true, {x, 300}, "end:3");
        CHECK(g.history().steps().size() == 1 && g.history().steps()[0].changes.size() == 1);
        CHECK(g.undo() && g.sheet() == small_sheet());

        // Many items at once, alternating moves: still one change each.
        document h;
        std::vector<item_ref> many;
        for (int i = 0; i < 300; ++i)
        {
            many.push_back(part_ref(h.add_part(make_part("R", {i * 400, 0}))));
            many.push_back(wire_ref(h.add_wire({i * 400, 100}, {i * 400 + 200, 100})));
        }
        const std::size_t steps = h.history().steps().size();
        for (int i = 0; i < 20; ++i)
            h.move(many, {0, 100}, "drag:many");
        CHECK(h.history().steps().size() == steps + 1);
        CHECK(h.history().steps().back().changes.size() == many.size());
    }

    // A quarter turn, or a mirror image, of a point about another.
    point turned(point p, point about, bool clockwise)
    {
        const point d = p - about;
        return about + (clockwise ? point{d.y(), -d.x()} : point{-d.y(), d.x()});
    }

    point reflected(point p, point about, bool vertical_axis)
    {
        return vertical_axis ? point{2 * about.x() - p.x(), p.y()} : point{p.x(), 2 * about.y() - p.y()};
    }

    // What `after` draws is `move` applied to what `before` draws: every
    // stroke point and every pin.
    template <typename Move>
    bool draws_as(const part &before, const part &after, Move move)
    {
        const symbol &s = find_symbol(before.symbol);
        bool ok = true;
        for (const auto &stroke : s.strokes)
            for (const point local : stroke)
                ok = ok && transform(after, local) == move(transform(before, local));
        for (const pin &p : s.pins)
            ok = ok && transform(after, p.position) == move(transform(before, p.position)) &&
                 transform_direction(after, p.direction) == move(transform(before, p.direction)) - move(transform(before, {0, 0}));
        return ok;
    }

    void turning()
    {
        const point pivot{300, -200};
        for (const char *name : {"R", "Q_NPN", "OpAmp", "LED", "Conn_02x03"})
        {
            for (std::uint8_t rotation = 0; rotation < 4; ++rotation)
            {
                for (const bool mirrored : {false, true})
                {
                    sheet s;
                    s.parts.insert(1, make_part(name, {1200, 700}, "X1", rotation, mirrored));
                    s.wires.insert(2, wire{{100, 0}, {300, 400}, {}});
                    s.labels.insert(3, make_label(label_kind::net, {-500, 100}, "N", rotation));
                    document d(s);
                    const std::vector<item_ref> all{part_ref(1), wire_ref(2), label_ref(3)};

                    // Rotation, both ways, is a true rotation of the drawing.
                    for (const bool clockwise : {false, true})
                    {
                        const part before = *d.find_part(1);
                        CHECK(d.rotate(all, pivot, clockwise) && d.undo_label() == "Rotate");
                        CHECK(draws_as(before, *d.find_part(1),
                                       [&](point p) { return turned(p, pivot, clockwise); }));
                        CHECK(d.find_wire(2)->a == turned({100, 0}, pivot, clockwise));
                        CHECK(d.find_label(3)->position == turned({-500, 100}, pivot, clockwise));
                        CHECK(d.find_label(3)->rotation == (rotation + (clockwise ? 3 : 1)) % 4);
                        CHECK(d.undo() && d.sheet() == s);
                    }

                    // Four turns are nothing.
                    for (int i = 0; i < 4; ++i)
                        d.rotate(all, pivot, false);
                    CHECK(d.sheet() == s);
                    d.rotate(all, pivot, true);
                    d.rotate(all, pivot, false);
                    CHECK(d.sheet() == s);

                    // Mirroring is a true mirror image, and twice is nothing.
                    for (const bool vertical : {true, false})
                    {
                        const part before = *d.find_part(1);
                        CHECK(d.mirror(all, pivot, vertical) && d.undo_label() == "Mirror");
                        CHECK(d.find_part(1)->mirrored != before.mirrored);
                        CHECK(draws_as(before, *d.find_part(1),
                                       [&](point p) { return reflected(p, pivot, vertical); }));
                        CHECK(d.find_wire(2)->b == reflected({300, 400}, pivot, vertical));
                        d.mirror(all, pivot, vertical);
                        CHECK(d.sheet() == s);
                    }
                }
            }
        }

        // Labels: the text runs the mirrored way.
        sheet s;
        s.labels.insert(1, make_label(label_kind::net, {0, 0}, "A", 0));
        s.labels.insert(2, make_label(label_kind::net, {0, 0}, "B", 1));
        document d(s);
        d.mirror(std::vector{label_ref(1), label_ref(2)}, {100, 0}, true);
        CHECK(d.find_label(1)->rotation == 2 && d.find_label(2)->rotation == 1);
        CHECK(d.find_label(1)->position == point{200, 0});
        d.undo();
        d.mirror(std::vector{label_ref(1), label_ref(2)}, {0, 100}, false);
        CHECK(d.find_label(1)->rotation == 0 && d.find_label(2)->rotation == 3);
        CHECK(d.find_label(2)->position == point{0, 200});

        // Nothing selected, or a pivot that changes nothing: no step.
        document e(small_sheet());
        CHECK(!e.rotate({}, {0, 0}) && !e.mirror({}, {0, 0}) && !e.can_undo());
    }

    void properties()
    {
        document d(small_sheet());
        const sheet start = d.sheet();

        // Typing into the value: one step.
        for (const char *text : {"1", "10", "10k", "10", "100", "100n"})
            CHECK(d.set_value(2, text, "type:value:2"));
        CHECK(d.history().steps().size() == 1 && d.undo_label() == "Edit value" && d.find_part(2)->value == "100n");
        CHECK(!d.set_value(2, "100n", "type:value:2")); // the same
        CHECK(!d.set_value(99, "x") && !d.set_value(3, "x")); // no such part
        CHECK(d.undo() && d.sheet() == start);

        // Typing that ends at the starting text leaves nothing.
        d.set_reference(1, "R10", "type:reference:1");
        d.set_reference(1, "R1", "type:reference:1");
        CHECK(!d.can_undo() && d.sheet() == start);
        CHECK(d.set_reference(1, "R5", "type:reference:1") && d.undo_label() == "Edit reference");

        // A different field is a different step.
        d.set_label_text(4, "MIDDLE", "type:text:4");
        CHECK(d.undo_label() == "Edit label" && d.find_label(4)->text == "MIDDLE" && d.history().steps().size() == 2);
        CHECK(!d.set_label_text(1, "x"));

        // Whole records.
        part r = *d.find_part(1);
        r.value = "4k7";
        r.library_part = "volt-resistors:r-4k7";
        CHECK(d.replace_part(1, r) && d.undo_label() == "Edit R5" && *d.find_part(1) == r);
        CHECK(!d.replace_part(1, r) && !d.replace_part(42, r));
        CHECK(d.replace_wire(3, wire{{100, 0}, {300, 100}, {}}) && d.undo_label() == "Edit wire");
        label l = *d.find_label(4);
        l.kind = label_kind::note;
        CHECK(d.replace_label(4, l) && d.find_label(4)->kind == label_kind::note);
        CHECK(!d.replace_label(4, l));

        while (d.undo())
        {
        }
        CHECK(d.sheet() == start && !d.modified());
    }

    void transactions()
    {
        document d(small_sheet());
        const sheet start = d.sheet();

        d.begin("Draw and place");
        CHECK(d.in_transaction() && !d.can_undo());
        const item_id w = d.add_wire({500, 0}, {700, 0});
        const item_id p = d.add_part(make_part("R", {800, 0}));
        d.move(std::vector{part_ref(p)}, {0, 100}, "ignored"); // joins, key unused
        CHECK(!d.undo()); // refused while open
        d.commit();
        CHECK(!d.in_transaction() && d.history().steps().size() == 1 && d.undo_label() == "Draw and place");
        CHECK(d.history().steps()[0].changes.size() == 2); // the move merged into the insert
        CHECK(d.find_part(p)->position == point{800, 100});
        const sheet drawn = d.sheet();
        CHECK(d.undo() && d.sheet() == start && d.redo() && d.sheet() == drawn);
        CHECK(d.find_wire(w) != nullptr);

        // Nested: one step, the outer label.
        d.begin("Outer");
        d.begin("Inner");
        d.set_value(1, "1k");
        d.commit();
        d.set_value(2, "1u");
        d.commit();
        CHECK(d.history().steps().size() == 2 && d.undo_label() == "Outer");

        // Cancelled: as it was, no step; ids handed out stay used.
        const sheet before = d.sheet();
        const item_id next = d.next_id();
        d.begin("Cancelled");
        d.add_part(make_part("C", {0, 1000}));
        d.erase(std::vector{part_ref(1)});
        d.rotate(std::vector{wire_ref(3)}, {0, 0});
        CHECK(d.modified());
        d.cancel();
        CHECK(!d.in_transaction() && d.sheet() == before && d.history().steps().size() == 2);
        CHECK(d.next_id() > next);
        d.cancel(); // nothing open: nothing

        // A transaction that comes to nothing adds no step.
        d.begin("Nothing");
        d.move(std::vector{part_ref(1)}, {100, 0});
        d.move(std::vector{part_ref(1)}, {-100, 0});
        d.commit();
        CHECK(d.history().steps().size() == 2 && d.sheet() == before);

        // A transaction committed with a key merges into the newest step
        // of that key.
        d.begin("Drag");
        d.move(std::vector{part_ref(1)}, {100, 0});
        d.commit("drag:t");
        d.begin("Drag");
        d.move(std::vector{part_ref(1)}, {100, 0});
        d.commit("drag:t");
        CHECK(d.history().steps().size() == 3 && d.find_part(1)->position == point{200, 0});
        CHECK(d.mark_saved() && !d.modified());
    }

    void pasting()
    {
        document d(small_sheet());
        d.add_part(make_part("R", {0, 400})); // R2
        const sheet start = d.sheet();

        const std::vector<item_ref> selection{part_ref(1), wire_ref(3), label_ref(4), part_ref(2)};
        const fragment copied = d.copy(selection);
        CHECK(copied.parts.size() == 2 && copied.wires.size() == 1 && copied.labels.size() == 1);
        CHECK(copied.parts[0].reference == "R1" && copied.parts[1].reference == "C1"); // sheet order
        CHECK(d.copy({}).empty() && d.copy(std::vector{part_ref(77)}).empty());

        const item_id next = d.next_id();
        const std::vector<item_ref> pasted = d.paste(copied, {1000, 0});
        CHECK(pasted.size() == 4 && d.undo_label() == "Paste 4 items" && d.history().steps().size() == 2);
        CHECK(pasted[0] == part_ref(next) && pasted[1] == part_ref(next + 1) && pasted[2] == wire_ref(next + 2) &&
              pasted[3] == label_ref(next + 3));
        // Fresh references: R1 and R2 are taken, so R3; C2.
        CHECK(d.find_part(next)->reference == "R3" && d.find_part(next + 1)->reference == "C2");
        CHECK(d.find_part(next)->position == point{1000, 0});
        CHECK(d.find_wire(next + 2)->a == point{1100, 0} && d.find_label(next + 3)->position == point{1200, 0});
        CHECK(d.find_part(next)->value == d.find_part(1)->value);

        // Pasted twice: numbered on.
        const std::vector<item_ref> again = d.paste(copied.parts, {}, {}, {0, 2000});
        CHECK(again.size() == 2 && d.find_part(again[0].id)->reference == "R4" &&
              d.find_part(again[1].id)->reference == "C3");
        CHECK(d.paste(fragment{}).empty() && d.history().steps().size() == 3);

        CHECK(d.undo() && d.undo() && d.sheet() == start);
    }

    // ---- tidying wires ------------------------------------------------------------

    std::size_t tidy(sheet s, sheet &result)
    {
        document d(std::move(s));
        const sheet start = d.sheet();
        const std::size_t removed = d.tidy_wires();
        result = d.sheet();
        if (removed > 0)
        {
            CHECK(d.history().steps().size() == 1 && d.undo_label() == "Tidy wires");
            CHECK(d.undo() && d.sheet() == start && d.redo() && d.sheet() == result);
        }
        else
            CHECK(!d.can_undo() && d.sheet() == start);
        return removed;
    }

    void tidying()
    {
        sheet result;

        // Of no length; duplicates either way round, the first staying.
        {
            sheet s;
            s.wires.insert(1, wire{{0, 0}, {0, 0}, {}});
            s.wires.insert(2, wire{{0, 0}, {200, 0}, {}});
            s.wires.insert(3, wire{{200, 0}, {0, 0}, {}});
            s.wires.insert(4, wire{{0, 0}, {200, 0}, {}});
            s.wires.insert(5, wire{{0, 0}, {0, 200}, {}});
            CHECK(tidy(s, result) == 3);
            CHECK(ids_of(result.wires) == (std::vector<item_id>{2, 5}));
        }
        // A straight run of three: one wire, the first, from end to end.
        {
            sheet s;
            s.wires.insert(7, wire{{200, 0}, {100, 0}, {}});
            s.wires.insert(3, wire{{200, 0}, {500, 0}, {}});
            s.wires.insert(9, wire{{0, 0}, {100, 0}, {}});
            CHECK(tidy(s, result) == 2);
            CHECK(ids_of(result.wires) == std::vector<item_id>{7});
            CHECK(*result.wires.find(7) == (wire{{500, 0}, {0, 0}, {}}));
        }
        // Diagonal.
        {
            sheet s;
            s.wires.insert(1, wire{{0, 0}, {100, 100}, {}});
            s.wires.insert(2, wire{{100, 100}, {300, 300}, {}});
            CHECK(tidy(s, result) == 1 && *result.wires.find(1) == (wire{{0, 0}, {300, 300}, {}}));
        }
        // Not joined: something else attached at the shared end.
        const auto straight = [] {
            sheet s;
            s.wires.insert(1, wire{{0, 0}, {200, 0}, {}});
            s.wires.insert(2, wire{{200, 0}, {400, 0}, {}});
            return s;
        };
        {
            sheet s = straight(); // a third wire's end
            s.wires.insert(3, wire{{200, 0}, {200, 300}, {}});
            CHECK(tidy(s, result) == 0);
        }
        {
            sheet s = straight(); // a pin (R's pin 1 at -100 from its origin)
            s.parts.insert(4, make_part("R", {300, 0}));
            CHECK(tidy(s, result) == 0);
        }
        {
            sheet s = straight(); // a label
            s.labels.insert(4, make_label(label_kind::net, {200, 0}, "X"));
            CHECK(tidy(s, result) == 0);
        }
        {
            sheet s = straight(); // a wire through the point: a T onto its inside
            s.wires.insert(3, wire{{200, -100}, {200, 100}, {}});
            CHECK(tidy(s, result) == 0);
        }
        {
            sheet s = straight(); // a part elsewhere and a label elsewhere do not matter
            s.parts.insert(4, make_part("R", {300, 500}));
            s.labels.insert(5, make_label(label_kind::net, {0, 0}, "X"));
            CHECK(tidy(s, result) == 1 && *result.wires.find(1) == (wire{{0, 0}, {400, 0}, {}}));
        }
        // Not in a line, or overlapping rather than continuing.
        {
            sheet s;
            s.wires.insert(1, wire{{0, 0}, {200, 0}, {}});
            s.wires.insert(2, wire{{200, 0}, {200, 200}, {}});
            CHECK(tidy(s, result) == 0);
        }
        {
            sheet s;
            s.wires.insert(1, wire{{0, 0}, {200, 0}, {}});
            s.wires.insert(2, wire{{0, 0}, {100, 0}, {}});
            CHECK(tidy(s, result) == 0);
        }
        // A wire of no length and a join, in one go.
        {
            sheet s;
            s.wires.insert(1, wire{{0, 0}, {100, 0}, {}});
            s.wires.insert(2, wire{{100, 0}, {200, 0}, {}});
            s.wires.insert(3, wire{{0, 50}, {0, 50}, {}});
            CHECK(tidy(s, result) == 2 && result.wires.size() == 1);
        }
        // Inside a transaction it joins it.
        {
            document d(straight());
            d.begin("Delete and tidy");
            d.add_wire({400, 0}, {600, 0});
            CHECK(d.tidy_wires() == 2);
            d.commit();
            CHECK(d.history().steps().size() == 1 && d.undo_label() == "Delete and tidy");
            CHECK(d.sheet().wires.size() == 1 && *d.sheet().wires.find(1) == (wire{{0, 0}, {600, 0}, {}}));
            CHECK(d.undo() && d.sheet() == straight());
        }
    }

    void saving_and_notification()
    {
        document d(small_sheet());
        int calls = 0;
        d.on_change([&] { ++calls; });
        CHECK(calls == 0);

        const std::uint64_t r0 = d.revision();
        d.add_wire({0, 500}, {100, 500});
        CHECK(calls == 1 && d.revision() > r0 && d.modified());

        // No change, no call.
        d.move(std::vector{part_ref(1)}, {0, 0});
        d.erase(std::vector{part_ref(99)});
        d.set_value(1, d.find_part(1)->value);
        CHECK(calls == 1);

        // One call per operation, not per record.
        d.move(std::vector{part_ref(1), part_ref(2), wire_ref(3)}, {0, 100});
        CHECK(calls == 2);

        CHECK(d.mark_saved() && !d.modified() && calls == 3);
        CHECK(d.undo() && d.modified() && calls == 4);
        CHECK(d.redo() && !d.modified());
        d.set_value(1, "1M");
        CHECK(d.modified());
        CHECK(d.undo() && !d.modified());

        // Inside a transaction: called per operation too, and modified.
        const int before = calls;
        d.begin("Gesture");
        d.move(std::vector{part_ref(1)}, {100, 0});
        d.move(std::vector{part_ref(1)}, {100, 0});
        CHECK(calls == before + 3 && d.modified() && !d.mark_saved());
        d.cancel();
        CHECK(!d.modified());

        // A callback that edits: called again after it returns, not inside.
        int depth = 0;
        int deepest = 0;
        int edits = 0;
        d.on_change([&] {
            ++depth;
            deepest = std::max(deepest, depth);
            if (edits++ < 3)
                d.set_value(2, "v" + std::to_string(edits), "type");
            --depth;
        });
        d.set_value(1, "x");
        CHECK(edits == 4 && deepest == 1 && d.find_part(2)->value == "v3");

        // A copy is independent, without the callback.
        d.on_change(nullptr);
        const document copy = d;
        d.add_wire({0, 900}, {100, 900});
        CHECK(copy.sheet() != d.sheet() && copy.history().steps().size() + 1 == d.history().steps().size());
        CHECK(copy.next_id() < d.next_id());
        document assigned;
        assigned = copy;
        CHECK(assigned.sheet() == copy.sheet() && assigned.can_undo() == copy.can_undo());
        CHECK(assigned.undo() && assigned.sheet() != copy.sheet());
    }

    // ---- a long random run --------------------------------------------------------------

    void random_edits()
    {
        std::mt19937 rng(20261122u);
        auto pick = [&rng](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };
        auto chance = [&rng](int percent) { return std::uniform_int_distribution<int>(0, 99)(rng) < percent; };
        auto coordinate = [&](int range) {
            return static_cast<int>(pick(static_cast<std::size_t>(2 * range + 1))) * 100 - range * 100;
        };
        auto random_point = [&] { return point{coordinate(20), coordinate(20)}; };

        document d(small_sheet(), history::stack_limits{.max_steps = 0, .max_cost = 0, .merge_window = 4096});
        std::vector<sheet> states{d.sheet()};
        const sheet initial = d.sheet();
        bool consistent = true;
        int operations = 0;
        int cancelled = 0;

        // Some items of the sheet, of any kind.
        auto some_items = [&] {
            std::vector<item_ref> all;
            for (const auto &e : d.sheet().parts)
                all.push_back(part_ref(e.id));
            for (const auto &e : d.sheet().wires)
                all.push_back(wire_ref(e.id));
            for (const auto &e : d.sheet().labels)
                all.push_back(label_ref(e.id));
            std::vector<item_ref> chosen;
            for (std::size_t i = 0, n = 1 + pick(4); i < n && !all.empty(); ++i)
                chosen.push_back(all[pick(all.size())]);
            if (chance(5))
                chosen.push_back(part_ref(999'999)); // not there
            return chosen;
        };
        auto some_part = [&]() -> item_id {
            if (d.sheet().parts.empty())
                return 0;
            return d.sheet().parts[pick(d.sheet().parts.size())].id;
        };
        const char *symbols[] = {"R", "C", "Q_NPN", "OpAmp", "GND", "Conn_01x03", "Mystery"};
        const char *keys[] = {"", "", "drag:a", "drag:b", "type"};

        auto random_operation = [&] {
            ++operations;
            switch (pick(15))
            {
            case 0:
            case 1:
                d.add_part(make_part(symbols[pick(7)], random_point(), chance(50) ? "" : "R?",
                                     static_cast<std::uint8_t>(pick(4)), chance(30)));
                break;
            case 2:
            case 3:
                d.add_wire(random_point(), random_point());
                break;
            case 4:
                d.add_label(make_label(chance(50) ? label_kind::net : label_kind::power, random_point(),
                                       chance(50) ? "GND" : "N" + std::to_string(pick(5))));
                break;
            case 5:
            case 6:
                d.erase(some_items());
                break;
            case 7:
            case 8:
                d.move(some_items(), {coordinate(2), coordinate(2)}, keys[pick(5)]);
                break;
            case 9:
                if (!d.sheet().wires.empty())
                    d.move_wire_end(d.sheet().wires[pick(d.sheet().wires.size())].id, chance(50), random_point(),
                                    keys[pick(5)]);
                break;
            case 10:
                d.rotate(some_items(), random_point(), chance(50));
                break;
            case 11:
                d.mirror(some_items(), random_point(), chance(50));
                break;
            case 12:
                d.set_value(some_part(), std::to_string(pick(20)), keys[pick(5)]);
                break;
            case 13:
                d.paste(d.copy(some_items()), {coordinate(5), coordinate(5)});
                break;
            default:
                d.tidy_wires();
                break;
            }
        };

        for (int round = 0; round < 1500; ++round)
        {
            // Now and then, back a few steps or forward again.
            if (chance(15))
            {
                for (std::size_t i = 0, n = pick(4); i < n; ++i)
                {
                    if (chance(60) ? !d.undo() : !d.redo())
                        break;
                }
                consistent = consistent && d.sheet() == states[d.history().position()];
            }
            if (chance(10))
                d.seal();

            const sheet before = d.sheet();
            const std::size_t position_before = d.history().position();
            const std::size_t steps_before = d.history().steps().size();
            if (chance(10))
            {
                // A gesture of a few operations, sometimes abandoned.
                d.begin("Gesture");
                for (std::size_t i = 0, n = 1 + pick(4); i < n; ++i)
                    random_operation();
                if (chance(30))
                {
                    d.cancel();
                    ++cancelled;
                    consistent = consistent && d.sheet() == before && d.history().position() == position_before &&
                                 d.history().steps().size() == steps_before;
                    continue;
                }
                d.commit(chance(30) ? "drag:a" : "");
            }
            else
                random_operation();

            // A new step, a merge into the newest (or its end), or nothing.
            if (d.history().position() == position_before && d.sheet() == before)
                continue;
            states.resize(d.history().position());
            states.push_back(d.sheet());
            consistent = consistent && d.history().position() + 1 == states.size();
        }
        CHECK(consistent);
        CHECK(operations > 1500 && cancelled > 0);
        CHECK(d.history().steps().size() > 100);

        while (d.redo())
            consistent = consistent && d.sheet() == states[d.history().position()];
        const sheet last = d.sheet();
        while (d.undo())
            consistent = consistent && d.sheet() == states[d.history().position()];
        CHECK(d.sheet() == initial && !d.modified());
        while (d.redo())
            consistent = consistent && d.sheet() == states[d.history().position()];
        CHECK(d.sheet() == last);
        CHECK(consistent);

        // Ids stayed unique across the tables.
        std::vector<item_id> all = ids_of(last.parts);
        for (const item_id id : ids_of(last.wires))
            all.push_back(id);
        for (const item_id id : ids_of(last.labels))
            all.push_back(id);
        std::ranges::sort(all);
        CHECK(std::ranges::adjacent_find(all) == all.end());
        CHECK(all.empty() || all.back() < d.next_id());
    }

} // namespace

int main()
{
    construction();
    adding();
    references();
    erasing();
    moving();
    dragging();
    turning();
    properties();
    transactions();
    pasting();
    tidying();
    saving_and_notification();
    random_edits();
    return volt::tests::result("schematic document");
}
