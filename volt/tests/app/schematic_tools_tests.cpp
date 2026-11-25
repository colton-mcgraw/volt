// The schematic editor's tools driven as the viewport drives them: scripted
// pointer and key events against real documents (the demo ladder and small
// sheets made here), each checked for the sheet it leaves, the undo history
// (how many steps, their labels, undo giving back the exact sheet, redo
// doing it again) and the selection. The select tool (clicks, cycling,
// toggling, double-click, drags as one step, Escape mid-drag, wire ends,
// rubber bands both ways), drawing wires (corners, finishing on pins, wire
// ends and insides of wires, checked with the netlist; Escape, right click,
// Backspace, one step for the lot), placing parts and labels, the commands,
// undo with a gesture open, switching documents, no document, wires that
// follow a drag, a turn mid-drag and a nudge (connections checked with the
// netlist, Alt to leave them, Ctrl off the grid), wire ends snapping to
// connections, parts placed, pasted or dropped on wires cutting them, and a
// long random run of events that must never leave a transaction open or a
// dead item selected.

#include "../math/check.hpp"

#include <app/ui/windows/schematic_tools.hpp>
#include <schematic/demo.hpp>
#include <schematic/document.hpp>
#include <schematic/hit.hpp>
#include <schematic/netlist.hpp>
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
using volt::ui::key;
using volt::ui::modifiers;
using volt::ui::mouse_button;
using tools_t = volt::ui::schematic_tools;

namespace
{
    constexpr float tol = 10.0f;

    math::vec2f at(float x, float y)
    {
        return math::vec2f{x, y};
    }

    item_ref part_ref(item_id id) { return {item_kind::part, id}; }
    item_ref wire_ref(item_id id) { return {item_kind::wire, id}; }
    item_ref label_ref(item_id id) { return {item_kind::label, id}; }

    part make_part(std::string symbol, point position, std::string reference, std::uint8_t rotation = 0)
    {
        part p;
        p.symbol = std::move(symbol);
        p.position = position;
        p.rotation = rotation;
        p.reference = std::move(reference);
        return p;
    }

    label make_label(label_kind kind, point position, std::string text)
    {
        label l;
        l.kind = kind;
        l.position = position;
        l.text = std::move(text);
        return l;
    }

    // R1 (1) at the origin and C1 (2) to its right, a wire (3) from R1's
    // pin 2 to C1's pin 1, and a net label (4) on it.
    sheet small_sheet()
    {
        sheet s;
        (void)s.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)s.parts.insert(2, make_part("C", {600, 0}, "C1"));
        (void)s.wires.insert(3, wire{{100, 0}, {500, 0}, {}});
        (void)s.labels.insert(4, make_label(label_kind::net, {300, 0}, "MID"));
        return s;
    }

    std::vector<item_ref> selection_of(const tools_t &t)
    {
        const auto s = t.selection();
        return {s.begin(), s.end()};
    }

    // A document and the tools on it, driven by events in sheet units.
    struct rig
    {
        document doc;
        tools_t tools;

        explicit rig(sheet s = {}) : doc(std::move(s)) { tools.set_document(&doc); }
        rig(const rig &) = delete;
        rig &operator=(const rig &) = delete;

        tools_t::input_result down(float x, float y, modifiers mods = modifiers::none, int clicks = 1,
                                   mouse_button button = mouse_button::left)
        {
            return tools.pointer_down(
                {.at = at(x, y), .tolerance = tol, .button = button, .mods = mods, .clicks = clicks});
        }

        tools_t::input_result move(float x, float y, modifiers mods = modifiers::none)
        {
            return tools.pointer_move({.at = at(x, y), .tolerance = tol, .button = mouse_button::left, .mods = mods});
        }

        tools_t::input_result up(float x, float y, modifiers mods = modifiers::none,
                                 mouse_button button = mouse_button::left)
        {
            return tools.pointer_up({.at = at(x, y), .tolerance = tol, .button = button, .mods = mods});
        }

        void click(float x, float y, modifiers mods = modifiers::none, int clicks = 1)
        {
            (void)down(x, y, mods, clicks);
            (void)up(x, y, mods);
        }

        void drag(float x0, float y0, float x1, float y1, modifiers mods = modifiers::none)
        {
            (void)down(x0, y0, mods);
            (void)move((x0 + x1) / 2.0f, (y0 + y1) / 2.0f, mods);
            (void)move(x1, y1, mods);
            (void)up(x1, y1, mods);
        }

        std::size_t steps() const { return doc.history().position(); }

        std::size_t overlay() const
        {
            std::vector<segment> out;
            tools.append_overlay(out, draw_style{});
            return out.size();
        }
    };

    // ---- the select tool --------------------------------------------------------

    void clicking()
    {
        rig r(small_sheet());
        CHECK(r.tools.active_tool() == tools_t::tool::select);
        CHECK(hit_test(r.doc.sheet(), at(0, 10), tol) == part_ref(1));

        // A click selects the topmost item, and makes no step.
        auto result = r.down(0, 10);
        CHECK(result.consumed && result.changed);
        CHECK(r.tools.in_gesture() && r.tools.selection().empty()); // decided on release
        result = r.up(0, 10);
        CHECK(result.consumed && result.changed);
        CHECK(selection_of(r.tools) == std::vector{part_ref(1)});
        CHECK(r.steps() == 0 && !r.tools.in_gesture());

        // Another item replaces it; empty space clears.
        r.click(150, 0);
        CHECK(selection_of(r.tools) == std::vector{wire_ref(3)});
        r.click(0, 2000);
        CHECK(r.tools.selection().empty());

        // A label sitting on the wire comes first.
        CHECK(hit_test(r.doc.sheet(), at(300, 5), tol) == label_ref(4));
        r.click(300, 5);
        CHECK(selection_of(r.tools) == std::vector{label_ref(4)});

        // Shift and Ctrl toggle; empty space leaves the selection.
        r.click(150, 0);
        r.click(0, 10, modifiers::shift);
        CHECK(selection_of(r.tools) == (std::vector{wire_ref(3), part_ref(1)}));
        r.click(150, 0, modifiers::control);
        CHECK(selection_of(r.tools) == std::vector{part_ref(1)});
        r.click(0, 2000, modifiers::shift);
        CHECK(selection_of(r.tools) == std::vector{part_ref(1)});

        // A click on a selected item of several selects just it.
        r.tools.select_all();
        CHECK(r.tools.selection().size() == 4);
        r.click(600, 0);
        CHECK(selection_of(r.tools) == std::vector{part_ref(2)});
        CHECK(r.steps() == 0 && r.doc.sheet() == small_sheet());

        // The selection changes the revision; a move over nothing, twice,
        // changes nothing the second time, and is not consumed.
        const std::uint64_t before = r.tools.revision();
        r.tools.clear_selection();
        CHECK(r.tools.revision() != before);
        (void)r.move(0, 3000);
        const auto still = r.move(1, 3001);
        CHECK(!still.changed && !still.consumed);
        CHECK(r.move(0, 10).changed && r.tools.hover() == part_ref(1)); // hovering R1
    }

    void cycling_and_double_click()
    {
        rig r(small_sheet());
        // Under (100, 0): the wire's end, then R1's body.
        CHECK(hit_test_all(r.doc.sheet(), at(100, 0), tol) == (std::vector{wire_ref(3), part_ref(1)}));
        r.click(100, 0);
        CHECK(selection_of(r.tools) == std::vector{wire_ref(3)});
        r.click(101, 1);
        CHECK(selection_of(r.tools) == std::vector{part_ref(1)});
        r.click(100, 0);
        CHECK(selection_of(r.tools) == std::vector{wire_ref(3)});

        // Elsewhere and back starts again at the top.
        r.click(600, 0);
        r.click(100, 0);
        CHECK(selection_of(r.tools) == std::vector{wire_ref(3)});

        // A double-click asks to edit what the first click selected.
        std::vector<item_ref> edits;
        r.tools.edit_requested = [&](item_ref item) { edits.push_back(item); };
        r.click(101, 1);
        CHECK(selection_of(r.tools) == std::vector{part_ref(1)});
        const auto result = r.down(101, 1, modifiers::none, 2);
        CHECK(result.consumed);
        CHECK(edits == std::vector{part_ref(1)});
        CHECK(selection_of(r.tools) == std::vector{part_ref(1)});
        CHECK(!r.tools.in_gesture());
        (void)r.up(101, 1);
        CHECK(r.steps() == 0);

        // On an unselected item it selects it too; on nothing, nothing.
        r.tools.clear_selection();
        (void)r.down(600, 0, modifiers::none, 2);
        (void)r.up(600, 0);
        CHECK(edits.size() == 2 && edits.back() == part_ref(2));
        CHECK(selection_of(r.tools) == std::vector{part_ref(2)});
        (void)r.down(0, 2000, modifiers::none, 2);
        (void)r.up(0, 2000);
        CHECK(edits.size() == 2);
    }

    void dragging()
    {
        rig r(small_sheet());
        const sheet before = r.doc.sheet();

        // A press on an unselected item and a drag moves it: one step.
        (void)r.down(0, 10);
        auto result = r.move(5, 12); // within the slop: still a press
        CHECK(result.consumed && !r.tools.dragging() && !r.doc.in_transaction());
        result = r.move(120, 12);
        CHECK(result.consumed && r.tools.dragging() && r.doc.in_transaction());
        CHECK(selection_of(r.tools) == std::vector{part_ref(1)});
        CHECK(r.doc.find_part(1)->position == point{100, 0});
        (void)r.move(160, 30);
        (void)r.move(205, 12);
        CHECK(r.doc.find_part(1)->position == point{200, 0});
        CHECK(r.overlay() == 0);
        result = r.up(205, 12);
        CHECK(result.consumed && result.changed);
        CHECK(!r.tools.in_gesture() && !r.doc.in_transaction());
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Move R1");
        // The wire on R1's pin 2 follows it: along itself, it shrinks.
        sheet moved = before;
        {
            part p = *before.parts.find(1);
            p.position = {200, 0};
            (void)moved.parts.replace(1, p);
            (void)moved.wires.replace(3, wire{{300, 0}, {500, 0}, {}});
        }
        CHECK(r.doc.sheet() == moved);
        CHECK(r.tools.undo() && r.doc.sheet() == before);
        CHECK(r.tools.redo() && r.doc.sheet() == moved);
        CHECK(selection_of(r.tools) == std::vector{part_ref(1)});

        // The next drag is a step of its own.
        r.drag(200, 10, 200, 210);
        CHECK(r.steps() == 2 && r.doc.find_part(1)->position == point{200, 200});
        CHECK(r.tools.undo() && r.doc.sheet() == moved);

        // A drag of a selected item moves the whole selection.
        rig all(small_sheet());
        all.tools.select_all();
        all.drag(600, 0, 600, 300);
        CHECK(all.steps() == 1 && all.doc.undo_label() == "Move 4 items");
        CHECK(all.doc.find_part(1)->position == point{0, 300} && all.doc.find_part(2)->position == point{600, 300});
        CHECK(all.doc.find_wire(3)->a == point{100, 300} && all.doc.find_label(4)->position == point{300, 300});
        CHECK(all.tools.selection().size() == 4);
        CHECK(all.tools.undo() && all.doc.sheet() == small_sheet());

        // Shift adds the item pressed and moves it with the rest.
        rig add(small_sheet());
        add.click(600, 0);
        add.drag(0, 10, 0, 110, modifiers::shift);
        CHECK(selection_of(add.tools) == (std::vector{part_ref(2), part_ref(1)}));
        CHECK(add.doc.find_part(1)->position == point{0, 100} && add.doc.find_part(2)->position == point{600, 100});
        CHECK(add.steps() == 1);
    }

    void drag_cancelled_and_returned()
    {
        rig r(small_sheet());
        // Something to redo, which a cancelled drag must not lose.
        r.tools.select(part_ref(2));
        CHECK(r.tools.rotate_selection());
        CHECK(r.tools.undo() && r.doc.can_redo());
        const sheet before = r.doc.sheet();

        (void)r.down(0, 10);
        (void)r.move(300, 10);
        CHECK(r.doc.find_part(1)->position == point{300, 0});
        CHECK(r.tools.hint().starts_with("Moving R1"));
        auto result = r.tools.key_down(key::escape);
        CHECK(result.consumed && result.changed);
        CHECK(!r.tools.in_gesture() && !r.doc.in_transaction());
        CHECK(r.doc.sheet() == before && r.steps() == 0 && r.doc.can_redo());
        // The release that follows does nothing.
        (void)r.move(400, 10);
        result = r.up(400, 10);
        CHECK(result.consumed);
        CHECK(r.doc.sheet() == before && r.steps() == 0 && r.doc.can_redo());

        // A right press cancels a drag too, and is consumed.
        (void)r.down(0, 10);
        (void)r.move(300, 10);
        result = r.down(300, 10, modifiers::none, 1, mouse_button::right);
        CHECK(result.consumed && !r.tools.dragging() && r.doc.sheet() == before);
        CHECK(r.up(300, 10, modifiers::none, mouse_button::right).consumed);
        (void)r.up(300, 10);
        CHECK(r.steps() == 0 && r.doc.can_redo());

        // A drag that comes back to where it started makes no step.
        (void)r.down(0, 10);
        (void)r.move(300, 10);
        (void)r.move(500, 200);
        (void)r.move(3, 8);
        (void)r.up(3, 8);
        CHECK(r.doc.sheet() == before && r.steps() == 0 && r.doc.can_redo());
        CHECK(!r.doc.in_transaction());
    }

    void drag_snapping()
    {
        // The displacement snaps, so an item off the grid keeps its offset.
        sheet s;
        (void)s.parts.insert(1, make_part("R", {30, 0}, "R1"));
        rig r(std::move(s));
        r.drag(30, 10, 220, 17);
        CHECK(r.doc.find_part(1)->position == point{230, 0});

        // Ctrl turns snapping off: whole units.
        r.drag(230, 10, 251, 18, modifiers::control);
        CHECK(r.doc.find_part(1)->position == point{251, 8});

        // A finer grid.
        r.tools.set_grid(25);
        r.drag(251, 10, 265, 10); // 14 rounds to 25
        CHECK(r.doc.find_part(1)->position == point{276, 8});
        r.tools.set_snap(false);
        r.drag(276, 10, 289, 10);
        CHECK(r.doc.find_part(1)->position == point{289, 8});
        CHECK(r.steps() == 4);
    }

    void wire_ends()
    {
        // An end of a wire drags alone; the step is "Move wire end".
        rig r(small_sheet());
        CHECK(hit_test(r.doc.sheet(), at(500, 0), tol) == wire_ref(3));
        (void)r.down(500, 0);
        (void)r.move(502, 100);
        CHECK(r.tools.dragging() && r.doc.in_transaction());
        (void)r.move(502, 197);
        (void)r.up(502, 197);
        CHECK(r.doc.find_wire(3)->a == point{100, 0} && r.doc.find_wire(3)->b == point{500, 200});
        CHECK(r.doc.find_part(2)->position == point{600, 0});
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Move wire end");
        CHECK(selection_of(r.tools) == std::vector{wire_ref(3)});
        CHECK(r.tools.undo() && r.doc.sheet() == small_sheet());

        // A corner: both ends there move together.
        sheet corner;
        (void)corner.wires.insert(1, wire{{0, 0}, {200, 0}, {}});
        (void)corner.wires.insert(2, wire{{200, 0}, {200, 200}, {}});
        rig c(corner);
        c.drag(200, 0, 300, 0);
        CHECK(c.doc.find_wire(1)->b == point{300, 0} && c.doc.find_wire(2)->a == point{300, 0});
        CHECK(c.doc.find_wire(1)->a == point{0, 0} && c.doc.find_wire(2)->b == point{200, 200});
        CHECK(c.steps() == 1);

        // Escape restores it.
        (void)c.down(300, 0);
        (void)c.move(400, 100);
        CHECK(c.doc.find_wire(1)->b == point{400, 100});
        (void)c.tools.cancel();
        (void)c.up(400, 100);
        CHECK(c.doc.find_wire(1)->b == point{300, 0} && c.steps() == 1);

        // An end dragged onto the other end: the wire goes, in the same step.
        sheet single;
        (void)single.wires.insert(1, wire{{0, 0}, {200, 0}, {}});
        rig z(single);
        z.drag(200, 0, 0, 0);
        CHECK(z.doc.sheet().wires.empty() && z.steps() == 1);
        CHECK(z.tools.selection().empty());
        CHECK(z.tools.undo() && z.doc.sheet() == single);

        // A wire selected with others moves with them, even from its end.
        rig w(small_sheet());
        w.tools.select_all();
        w.drag(500, 0, 500, 100);
        CHECK(w.doc.find_wire(3)->a == point{100, 100} && w.doc.find_wire(3)->b == point{500, 100});
        CHECK(w.doc.undo_label() == "Move 4 items");
    }

    void rubber_band()
    {
        // Left to right: what is wholly inside.
        rig r(small_sheet());
        (void)r.down(-150, -100);
        (void)r.move(0, 0);
        CHECK(r.tools.banding() && r.overlay() > 0 && !r.doc.in_transaction());
        CHECK(r.tools.cursor() == tools_t::pointer_shape::crosshair);
        (void)r.move(150, 100);
        CHECK(r.tools.hint().find("inside") != std::string::npos);
        (void)r.up(150, 100);
        CHECK(!r.tools.banding() && r.overlay() == 0);
        CHECK(selection_of(r.tools) == std::vector{part_ref(1)});

        // Right to left: what it touches.
        r.drag(150, 100, -150, -100);
        CHECK(selection_of(r.tools) == (std::vector{part_ref(1), wire_ref(3)}));

        // With Shift, added to the selection.
        r.click(600, 0);
        r.drag(-150, -100, 150, 100, modifiers::shift);
        CHECK(selection_of(r.tools) == (std::vector{part_ref(2), part_ref(1)}));

        // An empty band clears; nothing changed in the document.
        r.drag(-150, 1000, 150, 1100);
        CHECK(r.tools.selection().empty());
        CHECK(r.steps() == 0 && r.doc.sheet() == small_sheet());

        // Escape drops a band.
        (void)r.down(-150, -100);
        (void)r.move(150, 100);
        CHECK(r.tools.cancel() && !r.tools.banding());
        (void)r.up(150, 100);
        CHECK(r.tools.selection().empty());
    }

    void demo_drag()
    {
        rig r(demo_ladder());
        const sheet before = r.doc.sheet();
        CHECK(hit_test(before, at(-600, 210), tol) == part_ref(2)); // R1
        const netlist start = build_netlist(before);
        r.drag(-600, 210, -600, 410);
        CHECK(r.doc.find_part(2)->position == point{-600, 400});
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Move R1");
        // The wires followed R1: it is connected as it was, by a jog at
        // each pin (both far ends are held by more than can stretch).
        const netlist moved = build_netlist(r.doc.sheet());
        CHECK(moved.net_of_pin(2, "1") == moved.net_of_pin(1, "1"));
        CHECK(moved.nets.size() == start.nets.size() && moved.problems.empty());
        CHECK(r.doc.sheet().wires.size() == before.wires.size() + 2);
        CHECK(r.tools.undo() && r.doc.sheet() == before);

        // With Alt it moves alone, and is cut off.
        r.drag(-600, 210, -600, 410, modifiers::alt);
        CHECK(r.doc.find_part(2)->position == point{-600, 400} && r.doc.sheet().wires == before.wires);
        const netlist alone = build_netlist(r.doc.sheet());
        CHECK(alone.net_of_pin(2, "1") != alone.net_of_pin(1, "1"));
        CHECK(r.tools.undo() && r.doc.sheet() == before);
        const netlist back = build_netlist(r.doc.sheet());
        CHECK(back.net_of_pin(2, "1") == back.net_of_pin(1, "1"));
    }

    // ---- the wire tool ------------------------------------------------------------

    void wire_basics()
    {
        rig r;
        r.tools.set_tool(tools_t::tool::wire);
        CHECK(r.tools.cursor() == tools_t::pointer_shape::crosshair);

        // Two clicks: horizontal first, the pointer having left sideways.
        r.click(0, 0);
        CHECK(r.tools.drawing_wire() && r.steps() == 0);
        (void)r.move(300, 200);
        CHECK(r.tools.wire_preview() == (std::vector<point>{{0, 0}, {300, 0}, {300, 200}}));
        CHECK(r.overlay() > 0);
        CHECK(r.tools.hint().starts_with("Wire: click to add a corner"));
        r.click(300, 200);
        CHECK(r.tools.wire_points().size() == 3 && r.steps() == 0 && r.doc.sheet().wires.empty());

        // Escape ends it with what was fixed: one step.
        CHECK(r.tools.key_down(key::escape).consumed);
        CHECK(!r.tools.drawing_wire() && r.tools.active_tool() == tools_t::tool::wire);
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Draw wire");
        CHECK(r.doc.sheet().wires.size() == 2);
        CHECK(r.doc.sheet().wires[0].record.a == point{0, 0} && r.doc.sheet().wires[0].record.b == point{300, 0});
        CHECK(r.doc.sheet().wires[1].record.a == point{300, 0} && r.doc.sheet().wires[1].record.b == point{300, 200});

        // A second Escape goes back to Select.
        CHECK(r.tools.key_down(key::escape).consumed);
        CHECK(r.tools.active_tool() == tools_t::tool::select);
        CHECK(r.tools.undo() && r.doc.sheet() == sheet{});
        CHECK(r.tools.redo() && r.doc.sheet().wires.size() == 2);
    }

    void wire_corners()
    {
        // Leaving vertically: vertical first.
        rig r;
        r.tools.set_tool(tools_t::tool::wire);
        r.click(0, 0);
        (void)r.move(2, 50);
        (void)r.move(300, 200);
        CHECK(r.tools.wire_preview() == (std::vector<point>{{0, 0}, {0, 200}, {300, 200}}));
        // Back to the start forgets it; leaving sideways decides again.
        (void)r.move(3, 3);
        (void)r.move(60, 2);
        (void)r.move(300, 200);
        CHECK(r.tools.wire_preview() == (std::vector<point>{{0, 0}, {300, 0}, {300, 200}}));
        // The corner flips, and stays flipped.
        const std::uint64_t before = r.tools.revision();
        CHECK(r.tools.toggle_wire_corner() && r.tools.revision() != before);
        (void)r.move(300, 210);
        CHECK(r.tools.wire_preview() == (std::vector<point>{{0, 0}, {0, 200}, {300, 200}}));
        CHECK(r.tools.key_down(key::space).consumed);
        CHECK(r.tools.wire_preview() == (std::vector<point>{{0, 0}, {300, 0}, {300, 200}}));
        CHECK(r.tools.key_down(key::slash).consumed);
        r.click(300, 200);
        CHECK(r.tools.wire_points().size() == 3 && r.tools.wire_points()[1] == point{0, 200});

        // More corners, then a double-click finishes: one step for all.
        (void)r.move(600, 200);
        r.click(600, 200);
        (void)r.move(600, 500);
        r.click(600, 500);
        r.click(600, 500, modifiers::none, 2);
        CHECK(!r.tools.drawing_wire());
        CHECK(r.steps() == 1 && r.doc.sheet().wires.size() == 3);
        // (0,0)-(0,200) and (0,200)-(300,200), then (300,200)-(600,200)
        // joined to the one before it, then (600,200)-(600,500).
        CHECK(r.doc.sheet().wires[1].record.a == point{0, 200} && r.doc.sheet().wires[1].record.b == point{600, 200});
        CHECK(r.doc.sheet().wires[2].record.b == point{600, 500});
        CHECK(r.tools.undo() && r.doc.sheet().wires.empty());

        // Collinear clicks are tidied into one wire.
        rig line;
        line.tools.set_tool(tools_t::tool::wire);
        line.click(0, 0);
        line.click(100, 0);
        line.click(200, 0);
        line.click(200, 0, modifiers::none, 2);
        CHECK(line.doc.sheet().wires.size() == 1 && line.steps() == 1);
        CHECK(line.doc.sheet().wires[0].record.a == point{0, 0} && line.doc.sheet().wires[0].record.b == point{200, 0});

        // Off the grid with Ctrl; snapped otherwise (Alt is no matter).
        rig snap;
        snap.tools.set_tool(tools_t::tool::wire);
        snap.click(12, -7, modifiers::alt);
        CHECK(snap.tools.wire_points()[0] == point{0, 0});
        (void)snap.tools.cancel();
        snap.click(12, -7, modifiers::control);
        CHECK(snap.tools.wire_points()[0] == point{12, -7});
    }

    void wire_to_pin()
    {
        sheet s;
        (void)s.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)s.parts.insert(2, make_part("R", {600, 400}, "R2"));
        rig r(s);
        r.tools.set_tool(tools_t::tool::wire);

        // The idle wire tool marks the pin under the pointer.
        (void)r.move(500, 500);
        CHECK(r.overlay() == 0 && !r.tools.hover_connection());
        (void)r.move(103, 4);
        CHECK(r.tools.hover_connection() && r.tools.hover_connection()->at == point{100, 0});
        CHECK(r.tools.hover_connection()->kind == connection_kind::pin && r.overlay() > 0);

        // Starting on a pin within the tolerance; a click on a pin ends.
        r.click(103, 4);
        CHECK(r.tools.wire_points()[0] == point{100, 0});
        (void)r.move(497, 397);
        CHECK(r.tools.wire_preview().back() == point{500, 400});
        r.click(497, 397);
        CHECK(!r.tools.drawing_wire() && r.steps() == 1 && r.doc.undo_label() == "Draw wire");
        CHECK(r.doc.sheet().wires.size() == 2);
        const netlist n = build_netlist(r.doc.sheet());
        CHECK(n.net_of_pin(1, "2") && n.net_of_pin(1, "2") == n.net_of_pin(2, "1"));
        CHECK(n.net_of_pin(1, "1") != n.net_of_pin(2, "1"));
        CHECK(r.tools.undo() && r.doc.sheet() == s);
    }

    void wire_to_wire()
    {
        // Ending on a wire's end.
        {
            sheet s;
            (void)s.parts.insert(1, make_part("R", {0, 0}, "R1"));
            (void)s.wires.insert(2, wire{{300, 200}, {300, 600}, {}});
            rig r(s);
            r.tools.set_tool(tools_t::tool::wire);
            r.click(100, 0);
            (void)r.move(298, 203);
            r.click(298, 203);
            CHECK(!r.tools.drawing_wire() && r.steps() == 1);
            const netlist n = build_netlist(r.doc.sheet());
            CHECK(n.net_of_pin(1, "2") && n.net_of_pin(1, "2") == n.net_at({300, 600}));
            CHECK(r.tools.undo() && r.doc.sheet() == s);
        }

        // Ending inside a wire: a T, the wire not split.
        {
            sheet s;
            (void)s.parts.insert(1, make_part("R", {0, 0}, "R1"));
            (void)s.wires.insert(2, wire{{300, -400}, {300, 400}, {}});
            rig r(s);
            r.tools.set_tool(tools_t::tool::wire);
            r.click(100, 0);
            (void)r.move(303, 3);
            CHECK(r.tools.wire_preview().back() == point{300, 0});
            r.click(303, 3);
            CHECK(!r.tools.drawing_wire() && r.steps() == 1);
            CHECK(r.doc.sheet().wires.size() == 2 && *r.doc.find_wire(2) == *s.wires.find(2));
            const netlist n = build_netlist(r.doc.sheet());
            CHECK(n.net_of_wire(2) && n.net_of_wire(2) == n.net_of_pin(1, "2"));
        }

        // Starting inside a wire, ending on a pin.
        {
            sheet s;
            (void)s.parts.insert(1, make_part("R", {600, 0}, "R1"));
            (void)s.wires.insert(2, wire{{300, -400}, {300, 400}, {}});
            rig r(s);
            r.tools.set_tool(tools_t::tool::wire);
            r.click(302, -2);
            CHECK(r.tools.wire_points()[0] == point{300, 0});
            r.click(497, 2);
            CHECK(!r.tools.drawing_wire() && r.steps() == 1);
            const netlist n = build_netlist(r.doc.sheet());
            CHECK(n.net_of_wire(2) && n.net_of_wire(2) == n.net_of_pin(1, "1"));
        }
    }

    void wire_escapes()
    {
        rig r;
        r.tools.set_tool(tools_t::tool::wire);

        // Nothing fixed: no step.
        r.click(0, 0);
        (void)r.move(300, 0);
        CHECK(r.tools.cancel());
        CHECK(!r.tools.drawing_wire() && r.steps() == 0 && r.tools.active_tool() == tools_t::tool::wire);

        // A right click ends it, keeping what was fixed, and is consumed.
        r.click(0, 0);
        r.click(200, 0);
        (void)r.move(400, 100);
        auto result = r.down(400, 100, modifiers::none, 1, mouse_button::right);
        CHECK(result.consumed && result.changed);
        CHECK(r.up(400, 100, modifiers::none, mouse_button::right).consumed);
        CHECK(!r.tools.drawing_wire() && r.steps() == 1 && r.doc.sheet().wires.size() == 1);
        CHECK(r.doc.sheet().wires[0].record.b == point{200, 0});
        // With nothing open a right click is the context menu's.
        CHECK(!r.down(400, 100, modifiers::none, 1, mouse_button::right).consumed);
        CHECK(!r.up(400, 100, modifiers::none, mouse_button::right).consumed);
        CHECK(!r.down(400, 100, modifiers::none, 1, mouse_button::middle).consumed);
        CHECK(!r.up(400, 100, modifiers::none, mouse_button::middle).consumed);

        // Backspace takes the last point back.
        rig b;
        b.tools.set_tool(tools_t::tool::wire);
        b.click(0, 0);
        b.click(200, 0);
        b.click(200, 200);
        CHECK(b.tools.wire_points().size() == 3);
        CHECK(b.tools.key_down(key::backspace).consumed);
        CHECK(b.tools.wire_points().size() == 2 && b.steps() == 0);
        CHECK(b.tools.key_down(key::enter).consumed);
        CHECK(b.steps() == 1 && b.doc.sheet().wires.size() == 1);
        CHECK(b.doc.sheet().wires[0].record.b == point{200, 0});

        // ... the start too: no wire at all.
        b.click(0, 500);
        b.click(200, 500);
        CHECK(b.tools.remove_wire_point() && b.tools.remove_wire_point());
        CHECK(!b.tools.drawing_wire() && !b.tools.remove_wire_point() && b.steps() == 1);

        // A click on the last point adds nothing, a double-click there ends
        // a wire of nothing: no step.
        b.click(0, 800);
        b.click(0, 800);
        CHECK(b.tools.wire_points().size() == 1);
        b.click(0, 800, modifiers::none, 2);
        CHECK(!b.tools.drawing_wire() && b.steps() == 1);

        // A double-click with no wire drawn starts nothing.
        (void)b.down(0, 900, modifiers::none, 2);
        (void)b.up(0, 900);
        CHECK(!b.tools.drawing_wire());
    }

    // ---- placing ------------------------------------------------------------------------

    void placing_parts()
    {
        rig r;
        std::vector<item_ref> placed;
        r.tools.placed = [&](item_ref item) { placed.push_back(item); };

        // The part tool with nothing to place does nothing.
        r.tools.set_tool(tools_t::tool::part);
        CHECK(r.tools.hint().find("Parts panel") != std::string::npos);
        r.click(0, 0);
        CHECK(r.steps() == 0 && r.overlay() == 0);

        r.tools.begin_place_symbol("R");
        CHECK(r.tools.active_tool() == tools_t::tool::part && r.tools.placing());
        CHECK(r.tools.pending_part() && r.tools.pending_part()->reference == "R?");
        (void)r.tools.pointer_leave();
        CHECK(r.overlay() == 0); // no pointer
        CHECK(r.move(12, 7).changed);
        CHECK(r.overlay() > 0);
        CHECK(!r.move(30, 20).changed); // the ghost stays on (0, 0)
        CHECK(r.move(60, 20).changed && !r.move(61, 21).changed);
        r.click(12, 7);
        r.click(310, 0);
        r.click(590, 0);
        CHECK(r.steps() == 3 && r.doc.undo_label() == "Place R3");
        CHECK(r.doc.sheet().parts.size() == 3);
        CHECK(r.doc.sheet().parts[0].record.reference == "R1" && r.doc.sheet().parts[0].record.position == point{0, 0});
        CHECK(r.doc.sheet().parts[1].record.reference == "R2");
        CHECK(r.doc.sheet().parts[1].record.position == point{300, 0});
        CHECK(r.doc.sheet().parts[2].record.reference == "R3" && r.doc.sheet().parts[2].record.device == "resistor");
        CHECK(placed.size() == 3 && selection_of(r.tools) == std::vector{placed.back()});
        CHECK(r.tools.active_tool() == tools_t::tool::part);

        // A double-click's second press places nothing.
        (void)r.down(900, 0, modifiers::none, 2);
        (void)r.up(900, 0);
        CHECK(r.steps() == 3);

        // Turned and mirrored before placing: the document sees nothing.
        CHECK(r.tools.rotate_selection());
        CHECK(r.tools.key_down(key::x).consumed);
        CHECK(r.steps() == 3 && r.tools.pending_part()->rotation == 3 && r.tools.pending_part()->mirrored);
        r.click(0, 500);
        CHECK(r.doc.sheet().parts[3].record.rotation == 3 && r.doc.sheet().parts[3].record.mirrored);
        CHECK(r.doc.sheet().parts[3].record.reference == "R4");
        // The mirror image of a placed part as the document makes it.
        {
            document d;
            const item_id id = d.add_part(make_part("R", {}, "R?", 1));
            const item_ref item = part_ref(id);
            (void)d.mirror(std::span<const item_ref>(&item, 1), {}, true);
            CHECK(d.find_part(id)->rotation == 3 && d.find_part(id)->mirrored);
        }

        // Escape leaves the tool, undo takes the parts back one at a time.
        CHECK(r.tools.cancel() && r.tools.active_tool() == tools_t::tool::select);
        CHECK(r.tools.undo() && r.doc.sheet().parts.size() == 3);
        CHECK(r.tools.selection().empty());

        // A numbered reference is placed unnumbered.
        r.tools.begin_place_part(make_part("C", {500, 500}, "C7"));
        CHECK(r.tools.pending_part()->reference == "C?" && r.tools.pending_part()->position == point{0, 0});
        r.click(0, 1000);
        CHECK(r.doc.undo_label() == "Place C1");

        // A library part.
        library::part lp;
        lp.id = "res-10k";
        lp.symbol = "R";
        lp.value = "10k";
        lp.prefix = "R";
        lp.device = "resistor";
        const part t = tools_t::part_template(lp);
        CHECK(t.symbol == "R" && t.value == "10k" && t.reference == "R?" && t.library_part == "res-10k");
        r.tools.begin_place_part(lp);
        r.click(1000, 1000);
        const part &placed_part = *r.doc.find_part(placed.back().id);
        CHECK(placed_part.value == "10k" && placed_part.library_part == "res-10k" && placed_part.reference == "R4");
    }

    void power_is_a_label()
    {
        rig r;
        r.tools.begin_place_symbol("GND");
        CHECK(r.tools.active_tool() == tools_t::tool::power);
        CHECK(r.tools.label_text(label_kind::power) == "GND");
        r.click(0, 0);
        r.click(300, 0);
        CHECK(r.doc.sheet().parts.empty() && r.doc.sheet().labels.size() == 2);
        CHECK(r.doc.sheet().labels[1].record.kind == label_kind::power && r.doc.sheet().labels[1].record.text == "GND");
        CHECK(r.doc.undo_label() == "Place GND");

        r.tools.begin_place_symbol("PWR");
        CHECK(r.tools.label_text(label_kind::power) == "+5V");
        r.tools.begin_place_part(make_part("PWR", {}, "#PWR?"));
        CHECK(r.tools.active_tool() == tools_t::tool::power && !r.tools.pending_part());
        part supply = make_part("PWR", {}, "");
        supply.value = "+3V3";
        r.tools.begin_place_part(supply);
        CHECK(r.tools.label_text(label_kind::power) == "+3V3");

        // R turns the port; the text does not count up.
        CHECK(r.tools.key_down(key::r).consumed);
        CHECK(r.tools.label_rotation(label_kind::power) == 1);
        r.click(0, 500);
        r.click(300, 500);
        CHECK(r.doc.sheet().labels[2].record.text == "+3V3" && r.doc.sheet().labels[3].record.text == "+3V3");
        CHECK(r.doc.sheet().labels[3].record.rotation == 1 && r.doc.sheet().parts.empty());
        CHECK(r.steps() == 4);
    }

    void placing_labels()
    {
        sheet s;
        (void)s.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)s.labels.insert(2, make_label(label_kind::net, {0, 1000}, "NET3"));
        rig r(s);
        std::vector<item_ref> placed;
        r.tools.placed = [&](item_ref item) { placed.push_back(item); };

        r.tools.set_tool(tools_t::tool::label);
        CHECK(r.tools.label_text(label_kind::net) == "NET1");
        CHECK(r.tools.hint().find("NET1") != std::string::npos);
        (void)r.move(103, 2);
        CHECK(r.overlay() > 0 && r.tools.hover_connection());

        // On a pin within the tolerance, the next number afterwards.
        r.click(103, 2);
        CHECK(r.doc.sheet().labels.size() == 2 && r.doc.sheet().labels[1].record.position == point{100, 0});
        CHECK(r.doc.sheet().labels[1].record.text == "NET1");
        CHECK(placed.size() == 1 && selection_of(r.tools) == std::vector{placed[0]});
        CHECK(r.tools.label_text(label_kind::net) == "NET2");
        r.click(-103, 2);
        // NET3 is taken: NET4.
        CHECK(r.tools.label_text(label_kind::net) == "NET4");
        r.click(510, 220);
        CHECK(r.doc.sheet().labels[3].record.text == "NET4");
        CHECK(r.doc.sheet().labels[3].record.position == point{500, 200});

        // Text set by the editor; it counts on when it ends in digits.
        r.tools.set_label_text("DATA7");
        r.click(0, 300);
        CHECK(r.doc.sheet().labels[4].record.text == "DATA7" && r.tools.label_text(label_kind::net) == "DATA8");
        r.tools.set_label_text("SDA");
        r.click(0, 400);
        r.click(0, 500);
        CHECK(r.doc.sheet().labels[6].record.text == "SDA" && r.tools.label_text(label_kind::net) == "SDA");
        CHECK(r.steps() == 6 && r.doc.undo_label() == "Add label");

        // Notes: "Text", snapped to the grid, not to pins.
        r.tools.set_tool(tools_t::tool::text);
        CHECK(r.tools.label_text(label_kind::note) == "Text");
        r.click(103, 2);
        CHECK(r.doc.sheet().labels[7].record.kind == label_kind::note);
        CHECK(r.doc.sheet().labels[7].record.position == point{100, 0});
        CHECK(r.doc.sheet().labels[7].record.text == "Text");
        r.tools.set_label_text("Hello");
        CHECK(r.tools.label_text(label_kind::note) == "Hello" && r.tools.label_text(label_kind::net) == "SDA");

        // A ghost while the pointer is in; none once it leaves.
        (void)r.move(0, 0);
        CHECK(r.overlay() > 0);
        CHECK(r.tools.pointer_leave().changed);
        CHECK(r.overlay() == 0);
    }

    // ---- commands --------------------------------------------------------------------------

    void deleting_turning_mirroring()
    {
        rig r(small_sheet());
        CHECK(!r.tools.delete_selection());
        r.tools.select(std::vector{part_ref(1), part_ref(2)});
        CHECK(r.tools.delete_selection());
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Delete 2 items" && r.tools.selection().empty());
        CHECK(r.doc.sheet().parts.empty());
        CHECK(r.tools.undo() && r.doc.sheet() == small_sheet());

        r.tools.select(part_ref(1));
        CHECK(r.tools.key_down(key::del).consumed && r.doc.undo_label() == "Delete R1");
        CHECK(r.tools.undo());

        r.tools.select(part_ref(1));
        CHECK(r.tools.rotate_selection());
        CHECK(r.doc.undo_label() == "Rotate" && r.doc.find_part(1)->rotation == 1);
        CHECK(r.doc.find_part(1)->position == point{0, 0});
        CHECK(r.tools.key_down(key::r, modifiers::shift).consumed && r.doc.find_part(1)->rotation == 0);
        CHECK(r.tools.mirror_selection(true));
        CHECK(r.doc.undo_label() == "Mirror" && r.doc.find_part(1)->mirrored);
        CHECK(r.tools.key_down(key::y).consumed);
        CHECK(r.steps() == 4);
        while (r.tools.undo())
        {
        }
        CHECK(r.doc.sheet() == small_sheet());

        // With nothing selected, nothing.
        r.tools.clear_selection();
        CHECK(!r.tools.rotate_selection() && !r.tools.mirror_selection() && !r.tools.key_down(key::r).consumed);

        // While dragging, the drag's items turn within its one step.
        (void)r.down(0, 10);
        (void)r.move(200, 10);
        CHECK(r.tools.rotate_selection());
        CHECK(r.doc.find_part(1)->rotation == 1 && r.doc.in_transaction());
        (void)r.move(205, 12);
        (void)r.up(205, 12);
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Move R1");
        CHECK(r.doc.find_part(1)->rotation == 1 && r.doc.find_part(1)->position == point{200, 0});
        CHECK(r.tools.undo() && r.doc.sheet() == small_sheet());

        // Turned during a drag and dragged back: still a step (the turn).
        (void)r.down(0, 10);
        (void)r.move(200, 10);
        CHECK(r.tools.mirror_selection());
        (void)r.move(0, 10);
        (void)r.up(0, 10);
        CHECK(r.steps() == 1 && r.doc.find_part(1)->mirrored);
    }

    void copy_paste()
    {
        rig r(small_sheet());
        CHECK(!r.tools.copy() && !r.tools.paste() && !r.tools.can_paste());
        r.tools.select(std::vector{part_ref(1), wire_ref(3)});
        CHECK(r.tools.copy() && r.tools.can_paste());
        // About the selection's pivot, (200, 0).
        CHECK(r.tools.clipboard().parts.size() == 1 && r.tools.clipboard().parts[0].position == point{-200, 0});
        CHECK(r.tools.clipboard().wires.size() == 1 && r.tools.clipboard().wires[0].a == point{-100, 0});
        CHECK(r.steps() == 0);

        CHECK(r.tools.paste() && r.tools.pasting() && r.tools.placing());
        CHECK(r.tools.hint().starts_with("Paste 2 items"));
        (void)r.move(1003, 998);
        CHECK(r.overlay() > 0);
        CHECK(r.tools.rotate_selection() && r.tools.rotate_selection(true)); // and back
        (void)r.down(1003, 998);
        (void)r.up(1003, 998);
        CHECK(!r.tools.pasting() && r.overlay() == 0);
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Paste 2 items");
        const auto pasted = selection_of(r.tools);
        CHECK(pasted.size() == 2 && pasted[0].kind == item_kind::part && pasted[1].kind == item_kind::wire);
        CHECK(r.doc.find_part(pasted[0].id)->position == point{800, 1000});
        CHECK(r.doc.find_part(pasted[0].id)->reference == "R2");
        CHECK(r.doc.find_wire(pasted[1].id)->a == point{900, 1000});
        CHECK(r.doc.find_wire(pasted[1].id)->b == point{1300, 1000});
        CHECK(r.tools.undo() && r.doc.sheet() == small_sheet() && r.tools.selection().empty());

        // Escape abandons a paste; the tool underneath stays.
        r.tools.set_tool(tools_t::tool::wire);
        CHECK(r.tools.paste());
        (void)r.move(0, 500);
        CHECK(r.tools.key_down(key::escape).consumed && !r.tools.pasting());
        CHECK(r.tools.active_tool() == tools_t::tool::wire && r.steps() == 0);
        r.tools.set_tool(tools_t::tool::select);

        // A mirrored paste.
        CHECK(r.tools.paste());
        CHECK(r.tools.mirror_selection(true));
        r.click(0, 1000);
        const auto mirrored = selection_of(r.tools);
        CHECK(r.doc.find_part(mirrored[0].id)->position == point{200, 1000});
        CHECK(r.doc.find_part(mirrored[0].id)->mirrored);
        CHECK(r.tools.undo());

        // Cut: one step, and the clipboard holds what went.
        r.tools.select(part_ref(2));
        CHECK(r.tools.cut());
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Cut C1" && !r.doc.find_part(2));
        CHECK(r.tools.clipboard().parts.size() == 1 && r.tools.clipboard().parts[0].symbol == "C");
        CHECK(r.tools.selection().empty());
        CHECK(r.tools.undo() && r.doc.sheet() == small_sheet());

        // Duplicate: a ghost of the selection, the clipboard left alone.
        r.tools.select(part_ref(1));
        CHECK(r.tools.duplicate() && r.tools.pasting());
        CHECK(r.tools.clipboard().parts[0].symbol == "C");
        r.click(0, -500);
        CHECK(r.doc.undo_label() == "Paste 1 item" && r.tools.selection().size() == 1);
        CHECK(r.doc.find_part(r.tools.selection()[0].id)->position == point{0, -500});
        CHECK(r.doc.find_part(r.tools.selection()[0].id)->reference == "R2");

        // The clipboard as text, and back.
        const std::string text = r.tools.clipboard_text();
        CHECK(!text.empty());
        rig other;
        CHECK(!other.tools.set_clipboard_text("not a sheet") && !other.tools.can_paste());
        CHECK(other.tools.set_clipboard_text(text) && other.tools.clipboard() == r.tools.clipboard());
        CHECK(other.tools.paste());
        other.click(0, 0);
        CHECK(other.doc.sheet().parts.size() == 1 && other.doc.sheet().parts[0].record.reference == "C1");
    }

    void nudging()
    {
        rig r(small_sheet());
        r.tools.select(part_ref(1));
        CHECK(r.tools.nudge(1, 0));
        CHECK(r.tools.key_down(key::right).consumed);
        CHECK(r.tools.nudge(1, 0));
        CHECK(r.tools.key_down(key::up).consumed);
        CHECK(r.doc.find_part(1)->position == point{300, 100});
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Move R1");

        // Any other action ends the step.
        r.tools.select(part_ref(1));
        CHECK(r.tools.key_down(key::down).consumed && r.tools.key_down(key::left).consumed);
        CHECK(r.steps() == 2 && r.doc.find_part(1)->position == point{200, 0});
        r.click(0, 3000);
        r.tools.select(part_ref(1));
        r.tools.set_grid(50);
        CHECK(r.tools.nudge(0, -1));
        CHECK(r.steps() == 3 && r.doc.find_part(1)->position == point{200, -50});
        CHECK(r.tools.undo() && r.tools.undo() && r.doc.find_part(1)->position == point{300, 100});
        CHECK(r.tools.undo() && r.doc.sheet() == small_sheet());

        // Nothing selected: not consumed.
        r.tools.clear_selection();
        CHECK(!r.tools.nudge(1, 0) && !r.tools.key_down(key::left).consumed);
    }

    void cancel_levels()
    {
        rig r(small_sheet());
        r.tools.select(part_ref(1));
        r.tools.set_tool(tools_t::tool::wire);
        CHECK(selection_of(r.tools) == std::vector{part_ref(1)});
        r.click(0, 500);
        r.click(200, 500);
        CHECK(r.tools.cancel() && !r.tools.drawing_wire() && r.steps() == 1);
        CHECK(r.tools.cancel() && r.tools.active_tool() == tools_t::tool::select);
        CHECK(r.tools.cancel() && r.tools.selection().empty());
        CHECK(!r.tools.cancel() && !r.tools.key_down(key::escape).consumed);

        // Keys with Ctrl, Alt or Super are the menus'.
        r.tools.select(part_ref(1));
        CHECK(!r.tools.key_down(key::r, modifiers::control).consumed);
        CHECK(!r.tools.key_down(key::del, modifiers::alt).consumed);
        CHECK(!r.tools.key_down(key::c, modifiers::control).consumed);
        CHECK(r.steps() == 1);
    }

    void undo_mid_gesture()
    {
        rig r(small_sheet());
        CHECK(r.doc.add_wire({0, 500}, {100, 500}) != 0);
        CHECK(r.steps() == 1);

        // Mid-drag the document refuses undo; the tools cancel and undo.
        (void)r.down(0, 10);
        (void)r.move(200, 10);
        CHECK(r.doc.in_transaction() && !r.doc.can_undo());
        CHECK(r.tools.can_undo() && !r.tools.can_redo() && r.tools.undo_label() == "Draw wire");
        CHECK(r.tools.undo());
        CHECK(!r.doc.in_transaction() && !r.tools.in_gesture());
        CHECK(r.doc.sheet() == small_sheet() && r.steps() == 0);
        CHECK(r.tools.can_redo() && r.tools.redo_label() == "Draw wire");
        (void)r.up(200, 10);
        CHECK(r.doc.sheet() == small_sheet());

        // Mid-wire: the wire is discarded.
        CHECK(r.tools.redo());
        r.tools.set_tool(tools_t::tool::wire);
        r.click(0, 800);
        r.click(200, 800);
        CHECK(r.tools.undo() && !r.tools.drawing_wire());
        CHECK(r.doc.sheet() == small_sheet());

        // The selection loses what undo takes away.
        rig p;
        p.tools.begin_place_symbol("C");
        p.click(0, 0);
        CHECK(p.tools.selection().size() == 1);
        const std::uint64_t before = p.tools.revision();
        CHECK(p.doc.undo()); // from the menu, behind the tools' back
        CHECK(p.tools.revision() != before && p.tools.selection().empty());
        CHECK(p.doc.redo());
        CHECK(p.tools.selection().empty());
    }

    void switching_documents()
    {
        document a(small_sheet());
        document b(demo_ladder());
        tools_t t;
        t.set_document(&a);
        CHECK(t.document() == &a);

        // A drag open on one sheet is cancelled by switching.
        (void)t.pointer_down({.at = at(0, 10), .tolerance = tol});
        (void)t.pointer_move({.at = at(300, 10), .tolerance = tol});
        CHECK(a.in_transaction() && a.find_part(1)->position == point{300, 0});
        t.set_document(&b);
        CHECK(!a.in_transaction() && a.sheet() == small_sheet() && a.history().position() == 0);
        CHECK(t.selection().empty() && !t.in_gesture() && t.document() == &b);
        (void)t.pointer_up({.at = at(300, 10), .tolerance = tol});
        CHECK(b.history().position() == 0 && b.sheet() == demo_ladder());

        // A wire being drawn is discarded.
        t.set_tool(tools_t::tool::wire);
        (void)t.pointer_down({.at = at(0, 1000), .tolerance = tol});
        (void)t.pointer_up({.at = at(0, 1000), .tolerance = tol});
        (void)t.pointer_down({.at = at(300, 1000), .tolerance = tol});
        CHECK(t.drawing_wire());
        t.set_document(&a);
        CHECK(!t.drawing_wire() && b.history().position() == 0 && a.history().position() == 0);

        // The clipboard and a paste survive the switch.
        t.set_tool(tools_t::tool::select);
        t.select(part_ref(1));
        CHECK(t.copy());
        t.set_document(&b);
        CHECK(t.paste());
        t.set_document(&a);
        t.set_document(&b);
        CHECK(t.pasting());
        (void)t.pointer_down({.at = at(0, 1500), .tolerance = tol});
        CHECK(b.history().position() == 1 && b.undo_label() == "Paste 1 item");
        t.set_document(nullptr);
        CHECK(t.selection().empty() && t.document() == nullptr);
    }

    void no_document()
    {
        tools_t t;
        const tools_t::pointer_input e{.at = at(0, 0), .tolerance = tol};
        CHECK(!t.pointer_down(e).consumed && !t.pointer_move(e).consumed && !t.pointer_up(e).consumed);
        CHECK(!t.pointer_leave().consumed);
        CHECK(!t.delete_selection() && !t.rotate_selection() && !t.mirror_selection() && !t.copy() && !t.cut());
        CHECK(!t.paste() && !t.duplicate() && !t.nudge(1, 0) && !t.undo() && !t.redo());
        CHECK(!t.can_undo() && !t.can_redo() && t.undo_label().empty());
        t.select_all();
        t.select(part_ref(1));
        CHECK(t.selection().empty());
        std::vector<segment> out;
        t.append_overlay(out, draw_style{});
        CHECK(out.empty() && t.hint() == "No sheet open");
        CHECK(!t.cancel());
        t.set_tool(tools_t::tool::wire);
        CHECK(!t.key_down(key::space).consumed && !t.finish_wire());
        CHECK(t.key_down(key::escape).consumed && t.active_tool() == tools_t::tool::select);
        t.begin_place_symbol("R");
        CHECK(t.active_tool() == tools_t::tool::part && t.label_text(label_kind::net) == "NET1");

        // An empty sheet.
        rig r;
        r.click(0, 0);
        r.drag(0, 0, 500, 500);
        r.tools.select_all();
        CHECK(r.tools.selection().empty() && r.steps() == 0);
        CHECK(!r.tools.rotate_selection() && !r.tools.copy() && !r.tools.duplicate());
    }

    void out_of_order_and_underneath()
    {
        rig r(small_sheet());
        // A release with no press, a move with none.
        CHECK(!r.up(0, 0).consumed);
        (void)r.move(0, 10);
        CHECK(r.tools.hover() == part_ref(1));
        CHECK(r.tools.cursor() == tools_t::pointer_shape::move);
        CHECK(!r.tools.pointer_leave().consumed && !r.tools.hover());

        // The item pressed goes before the press becomes a drag: a band.
        (void)r.down(0, 10);
        const item_ref gone = part_ref(1);
        CHECK(r.doc.erase(std::span<const item_ref>(&gone, 1)) == 1);
        (void)r.move(200, 200);
        CHECK(r.tools.banding());
        (void)r.up(200, 200);
        CHECK(!r.tools.in_gesture() && !r.doc.in_transaction());
        CHECK(r.doc.undo());

        // Someone else cancels the drag's transaction.
        (void)r.down(0, 10);
        (void)r.move(200, 10);
        CHECK(r.doc.in_transaction());
        r.doc.cancel();
        (void)r.move(300, 10);
        CHECK(!r.tools.dragging());
        CHECK(r.up(300, 10).consumed);
        CHECK(!r.doc.in_transaction() && r.doc.sheet() == small_sheet());

        // Two presses with no release between: the first drag ends.
        (void)r.down(0, 10);
        (void)r.move(200, 10);
        (void)r.down(600, 0);
        CHECK(!r.doc.in_transaction() && r.doc.find_part(1)->position == point{200, 0});
        (void)r.up(600, 0);
        CHECK(selection_of(r.tools) == std::vector{part_ref(2)});

        // Hover over an item the document then loses.
        (void)r.move(600, 0);
        CHECK(r.tools.hover() == part_ref(2));
        r.tools.select(part_ref(2));
        CHECK(r.tools.delete_selection());
        CHECK(!r.tools.hover());
    }

    void overlay_states()
    {
        rig r(small_sheet());
        // Select: nothing, pressed: nothing, dragging: nothing (the sheet
        // itself moves), band: the rectangle.
        (void)r.move(0, 10);
        CHECK(r.overlay() == 0);
        (void)r.down(0, 10);
        CHECK(r.overlay() == 0);
        (void)r.move(200, 10);
        CHECK(r.overlay() == 0);
        (void)r.up(200, 10);
        (void)r.down(0, 3000);
        (void)r.move(100, 3100);
        CHECK(r.overlay() == 4);
        (void)r.up(100, 3100);
        CHECK(r.overlay() == 0);

        // Wire: a marker over a connection only; the preview while drawing.
        r.tools.set_tool(tools_t::tool::wire);
        (void)r.move(0, 3000);
        CHECK(r.overlay() == 0);
        (void)r.move(500, 2);
        CHECK(r.overlay() > 0);
        r.click(0, 3000);
        CHECK(r.overlay() == 0); // a path of one point draws nothing
        (void)r.move(300, 3000);
        CHECK(r.overlay() > 0);
        (void)r.tools.cancel();

        // Placing: a ghost while the pointer is in.
        r.tools.begin_place_symbol("R");
        CHECK(r.overlay() > 0);
        (void)r.tools.pointer_leave();
        CHECK(r.overlay() == 0);
        (void)r.move(0, 3000);
        CHECK(r.overlay() > 0);
        CHECK(r.tools.cancel() && r.overlay() == 0);
        r.tools.set_tool(tools_t::tool::power);
        CHECK(r.overlay() > 0);
        r.tools.set_tool(tools_t::tool::select);
        CHECK(r.overlay() == 0);
        r.tools.select(part_ref(1));
        CHECK(r.tools.duplicate() && r.overlay() > 0);
        (void)r.tools.cancel();
        CHECK(r.overlay() == 0);
    }

    // ---- wires that follow, cut and snap ------------------------------------------------

    // The wires' records in table order, ids left out: what two runs that
    // number new wires differently have in common.
    std::vector<wire> wires_of(const sheet &s)
    {
        std::vector<wire> out;
        for (const auto &e : s.wires)
            out.push_back(e.record);
        return out;
    }

    bool all_orthogonal(const sheet &s)
    {
        for (const auto &e : s.wires)
            if (e.record.a.x() != e.record.b.x() && e.record.a.y() != e.record.b.y())
                return false;
        return true;
    }

    bool any_wire_of_no_length(const sheet &s)
    {
        for (const auto &e : s.wires)
            if (e.record.a == e.record.b)
                return true;
        return false;
    }

    // Every pin's net, as the names of the other pins on it, for comparing
    // the connections of two sheets of the same parts.
    std::vector<std::vector<pin_ref>> pin_nets(const sheet &s)
    {
        const netlist nets = build_netlist(s);
        std::vector<std::vector<pin_ref>> out;
        for (const auto &e : s.parts)
            for (const placed_pin &p : placed_pins(e.record))
                out.push_back(nets.nets[*nets.net_of_pin(e.id, p.pin->number)].pins);
        return out;
    }

    // R1 (1) at the origin between R2 (2) on the left and a free corner on
    // the right: a wire (3) from R2's pin 2 to R1's pin 1, a wire (4) from
    // R1's pin 2 to (400, 0), and a wire (5) from there down to (400, -300).
    sheet follow_sheet()
    {
        sheet s;
        (void)s.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)s.parts.insert(2, make_part("R", {-500, 0}, "R2"));
        (void)s.wires.insert(3, wire{{-400, 0}, {-100, 0}, {}});
        (void)s.wires.insert(4, wire{{100, 0}, {400, 0}, {}});
        (void)s.wires.insert(5, wire{{400, 0}, {400, -300}, {}});
        return s;
    }

    void wires_follow_a_drag()
    {
        // Along the wires: they stretch and shrink.
        rig r(follow_sheet());
        const sheet before = r.doc.sheet();
        const auto nets = pin_nets(before);
        r.drag(0, 10, 100, 10);
        CHECK(r.doc.find_part(1)->position == point{100, 0});
        CHECK(*r.doc.find_wire(3) == (wire{{-400, 0}, {0, 0}, {}}));
        CHECK(*r.doc.find_wire(4) == (wire{{200, 0}, {400, 0}, {}}));
        CHECK(*r.doc.find_wire(5) == before.wires[2].record);
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Move R1" && pin_nets(r.doc.sheet()) == nets);
        CHECK(r.tools.undo() && r.doc.sheet() == before);

        // Across them: wire 4 slides up, wire 5 stretching to take it; wire
        // 3 cannot (R2's pin holds its far end), so it gets a jog at R1's
        // pin 1. One step, connected as before, all orthogonal.
        r.drag(0, 10, 0, 110);
        CHECK(r.doc.find_part(1)->position == point{0, 100});
        CHECK(*r.doc.find_wire(4) == (wire{{100, 100}, {400, 100}, {}}));
        CHECK(*r.doc.find_wire(5) == (wire{{400, 100}, {400, -300}, {}}));
        CHECK(*r.doc.find_wire(3) == before.wires[0].record);
        CHECK(r.doc.sheet().wires.size() == 4);
        CHECK(r.doc.sheet().wires[3].record == (wire{{-100, 0}, {-100, 100}, {}}));
        CHECK(r.steps() == 1 && pin_nets(r.doc.sheet()) == nets && all_orthogonal(r.doc.sheet()));
        const std::vector<wire> direct = wires_of(r.doc.sheet());
        CHECK(r.tools.undo() && r.doc.sheet() == before);

        // Both ways at once: wire 3 stretches along and jogs across, wire 4
        // shrinks along and slides.
        r.drag(0, 10, 100, 210);
        CHECK(*r.doc.find_wire(3) == (wire{{-400, 0}, {0, 0}, {}}));
        CHECK(r.doc.sheet().wires[3].record == (wire{{0, 0}, {0, 200}, {}}));
        CHECK(*r.doc.find_wire(4) == (wire{{200, 200}, {400, 200}, {}}));
        CHECK(*r.doc.find_wire(5) == (wire{{400, 200}, {400, -300}, {}}));
        CHECK(pin_nets(r.doc.sheet()) == nets && all_orthogonal(r.doc.sheet()));
        CHECK(r.tools.undo() && r.doc.sheet() == before);

        // Wandering about on the way makes no difference: every pointer
        // move starts again from the sheet the drag began with.
        (void)r.down(0, 10);
        for (const auto &[x, y] : {std::pair{300.0f, 410.0f}, {-200.0f, -190.0f}, {500.0f, 10.0f}, {0.0f, 610.0f},
                                   {-300.0f, 210.0f}, {0.0f, 110.0f}})
            (void)r.move(x, y);
        CHECK(r.doc.in_transaction() && r.tools.dragging() && !r.tools.detached());
        (void)r.up(0, 110);
        CHECK(wires_of(r.doc.sheet()) == direct && r.steps() == 1);
        CHECK(r.tools.undo() && r.doc.sheet() == before);

        // Escape gives back exactly the sheet, with no step; so does a
        // right press; a drag back to the start makes none.
        (void)r.down(0, 10);
        (void)r.move(200, 310);
        CHECK(r.doc.sheet().wires.size() > before.wires.size());
        CHECK(r.tools.key_down(key::escape).consumed && r.doc.sheet() == before && r.steps() == 0);
        (void)r.up(200, 310);
        (void)r.down(0, 10);
        (void)r.move(200, 310);
        CHECK(r.down(200, 310, modifiers::none, 1, mouse_button::right).consumed);
        (void)r.up(200, 310, modifiers::none, mouse_button::right);
        (void)r.up(200, 310);
        CHECK(r.doc.sheet() == before && r.steps() == 0);
        (void)r.down(0, 10);
        (void)r.move(200, 310);
        (void)r.move(0, 12);
        (void)r.up(0, 12);
        CHECK(r.doc.sheet() == before && r.steps() == 0 && !r.doc.can_undo());

        // Alt at the press: R1 moves alone, the wires stay; the hint says so.
        (void)r.down(0, 10, modifiers::alt);
        (void)r.move(0, 110); // Alt let go on the way: still detached
        CHECK(r.tools.detached() && r.tools.hint().find("leaving its wires") != std::string::npos);
        (void)r.up(0, 110);
        CHECK(r.doc.find_part(1)->position == point{0, 100} && r.doc.sheet().wires == before.wires);
        CHECK(r.steps() == 1 && r.tools.undo() && r.doc.sheet() == before);
    }

    void follow_kinds()
    {
        // A diagonal wire just follows with its end.
        sheet diagonal;
        (void)diagonal.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)diagonal.wires.insert(2, wire{{100, 0}, {400, 300}, {}});
        rig d(diagonal);
        d.drag(0, 10, 0, 110);
        CHECK(*d.doc.find_wire(2) == (wire{{100, 100}, {400, 300}, {}}) && d.doc.sheet().wires.size() == 1);

        // A wire between two moved parts moves whole; a wire from one of
        // them to nothing stretches along the move.
        sheet pair;
        (void)pair.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)pair.parts.insert(2, make_part("R", {400, 0}, "R2"));
        (void)pair.wires.insert(4, wire{{100, 0}, {300, 0}, {}});
        (void)pair.wires.insert(5, wire{{500, 0}, {500, -200}, {}});
        rig w(pair);
        w.tools.select(std::vector{part_ref(1), part_ref(2)});
        const auto nets = pin_nets(pair);
        w.drag(400, 10, 400, 210);
        CHECK(*w.doc.find_wire(4) == (wire{{100, 200}, {300, 200}, {}}));
        CHECK(*w.doc.find_wire(5) == (wire{{500, 200}, {500, -200}, {}}));
        CHECK(w.doc.undo_label() == "Move 2 items" && pin_nets(w.doc.sheet()) == nets);

        // A net label on a moved pin goes with it; a wire end on a moved
        // label follows the label.
        sheet labelled;
        (void)labelled.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)labelled.labels.insert(2, make_label(label_kind::net, {100, 0}, "OUT"));
        (void)labelled.labels.insert(3, make_label(label_kind::net, {0, 500}, "IN"));
        (void)labelled.wires.insert(4, wire{{0, 500}, {0, 800}, {}});
        rig l(labelled);
        l.drag(0, 10, 0, 110);
        CHECK(l.doc.find_label(2)->position == point{100, 100});
        l.tools.select(label_ref(3));
        (void)l.down(0, 505);
        (void)l.move(0, 400);
        (void)l.up(0, 400);
        CHECK(l.doc.find_label(3)->position == point{0, 400} && *l.doc.find_wire(4) == (wire{{0, 400}, {0, 800}, {}}));

        // A pin on another part's pin (no wire) stays connected by a new
        // wire from the pin that stays to the one that moved.
        sheet touching;
        (void)touching.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)touching.parts.insert(2, make_part("R", {200, 0}, "R2"));
        rig t(touching);
        const auto touching_nets = pin_nets(touching);
        t.drag(0, 10, 0, 210);
        CHECK(t.doc.sheet().wires.size() == 1 && t.doc.sheet().wires[0].record == (wire{{100, 0}, {100, 200}, {}}));
        CHECK(pin_nets(t.doc.sheet()) == touching_nets);

        // A selected wire dragged off the pins it joined: wires drawn from
        // the pins to its new ends.
        sheet between;
        (void)between.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)between.parts.insert(2, make_part("R", {400, 0}, "R2"));
        (void)between.wires.insert(3, wire{{100, 0}, {300, 0}, {}});
        rig b(between);
        b.drag(200, 0, 200, 200);
        CHECK(*b.doc.find_wire(3) == (wire{{100, 200}, {300, 200}, {}}) && b.doc.sheet().wires.size() == 3);
        CHECK(pin_nets(b.doc.sheet()) == pin_nets(between) && all_orthogonal(b.doc.sheet()));
        CHECK(!any_wire_of_no_length(b.doc.sheet()));
    }

    void turning_while_dragging()
    {
        // R1 with wires off both pins, turned while being dragged: the
        // wires follow the turned pins, still connected and orthogonal, in
        // the drag's one step.
        rig r(follow_sheet());
        const sheet before = r.doc.sheet();
        const auto nets = pin_nets(before);
        (void)r.down(0, 10);
        (void)r.move(0, 210);
        CHECK(r.tools.rotate_selection());
        CHECK(r.doc.find_part(1)->rotation == 1 && r.doc.in_transaction());
        (void)r.move(0, 310);
        CHECK(r.tools.key_down(key::x, modifiers::alt).consumed); // Alt does not stop it mid-drag
        (void)r.up(0, 310);
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Move R1");
        CHECK(r.doc.find_part(1)->position == point{0, 300} && r.doc.find_part(1)->mirrored);
        CHECK(pin_nets(r.doc.sheet()) == nets && all_orthogonal(r.doc.sheet()));
        CHECK(!any_wire_of_no_length(r.doc.sheet()));
        CHECK(r.tools.undo() && r.doc.sheet() == before);

        // Turned and dragged back where it began: a step all the same.
        (void)r.down(0, 10);
        (void)r.move(0, 210);
        CHECK(r.tools.rotate_selection());
        (void)r.move(0, 10);
        (void)r.up(0, 10);
        CHECK(r.steps() == 1 && r.doc.find_part(1)->rotation == 1 && pin_nets(r.doc.sheet()) == nets);
    }

    void dragging_wire_ends_snaps()
    {
        // A wire end dragged near another wire's end off the grid snaps to
        // it, marked; to a pin; to a point inside a wire; not to its own
        // wire.
        sheet s;
        (void)s.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)s.wires.insert(2, wire{{150, 550}, {450, 550}, {}});
        (void)s.wires.insert(3, wire{{-600, -400}, {600, -400}, {}});
        (void)s.wires.insert(4, wire{{300, 300}, {500, 300}, {}});
        rig r(s);
        (void)r.down(300, 300);
        (void)r.move(145, 553); // nearer the end than any point inside wire 2

        CHECK(r.tools.drag_connection() && r.tools.drag_connection()->at == point{150, 550});
        CHECK(r.tools.drag_connection()->kind == connection_kind::wire_end);
        CHECK(r.doc.find_wire(4)->a == point{150, 550} && r.overlay() > 0);
        (void)r.move(104, 6);
        CHECK(r.doc.find_wire(4)->a == point{100, 0} && r.tools.drag_connection()->kind == connection_kind::pin);
        (void)r.move(203, -395);
        CHECK(r.doc.find_wire(4)->a == point{200, -400});
        CHECK(r.tools.drag_connection()->kind == connection_kind::wire_inside);
        (void)r.move(497, 303); // its own other end: no connection, the grid
        CHECK(!r.tools.drag_connection() && r.doc.find_wire(4)->a == point{500, 300} && r.overlay() == 0);
        (void)r.move(258, 1203); // nothing near: the grid; Ctrl: whole units
        CHECK(r.doc.find_wire(4)->a == point{300, 1200});
        (void)r.move(258, 1203, modifiers::control);
        CHECK(r.doc.find_wire(4)->a == point{258, 1203});
        (void)r.move(104, 6, modifiers::control); // connections win, Ctrl or not
        (void)r.up(104, 6, modifiers::control);
        CHECK(r.doc.find_wire(4)->a == point{100, 0} && r.steps() == 1 && r.doc.undo_label() == "Move wire end");
        CHECK(!r.tools.drag_connection());
        const netlist nets = build_netlist(r.doc.sheet());
        CHECK(nets.net_of_wire(4) == nets.net_of_pin(1, "2"));
    }

    // A wire through where a resistor lands: the resistor ends up in
    // series, not shorted by it.
    void parts_dropped_on_wires_cut_them()
    {
        sheet line;
        (void)line.wires.insert(1, wire{{-500, 0}, {500, 0}, {}});
        // Two collinear wires elsewhere, which only a whole-sheet tidy
        // would join: every tidy here is scoped, so they stay.
        (void)line.wires.insert(2, wire{{-500, 2000}, {0, 2000}, {}});
        (void)line.wires.insert(3, wire{{0, 2000}, {500, 2000}, {}});

        // Placing.
        rig r(line);
        r.tools.begin_place_symbol("R");
        r.click(0, 0);
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Place R1");
        CHECK(r.doc.sheet().wires.size() == 4);
        CHECK(*r.doc.find_wire(1) == (wire{{-500, 0}, {-100, 0}, {}}));
        CHECK(r.doc.sheet().wires[3].record == (wire{{100, 0}, {500, 0}, {}}));
        {
            const item_id id = r.doc.sheet().parts[0].id;
            const netlist nets = build_netlist(r.doc.sheet());
            CHECK(nets.net_of_pin(id, "1") != nets.net_of_pin(id, "2"));
            CHECK(nets.net_of_pin(id, "1") == nets.net_of_wire(1));
        }
        CHECK(r.doc.find_wire(2) && r.doc.find_wire(3));
        CHECK(r.tools.undo() && r.doc.sheet() == line);

        // Placed across the wire (turned), it touches one pin at most: left
        // alone.
        CHECK(r.tools.rotate_selection());
        r.click(0, 100);
        CHECK(r.doc.sheet().wires == line.wires);
        CHECK(r.tools.undo());

        // Pasting.
        sheet one;
        (void)one.parts.insert(1, make_part("C", {0, 3000}, "C1"));
        rig p(one);
        p.tools.select(part_ref(1));
        CHECK(p.tools.copy());
        (void)p.doc.add_wire({-500, 0}, {500, 0});
        const sheet pasted_on = p.doc.sheet();
        CHECK(p.tools.paste());
        p.click(0, 0);
        CHECK(p.doc.undo_label() == "Paste 1 item");
        {
            const item_id id = p.tools.selection()[0].id;
            const netlist nets = build_netlist(p.doc.sheet());
            CHECK(nets.net_of_pin(id, "1") != nets.net_of_pin(id, "2"));
            CHECK(p.doc.sheet().wires.size() == 2);
        }
        CHECK(p.tools.undo() && p.doc.sheet() == pasted_on);

        // A part pasted with a wire through its pins keeps that wire.
        rig q(line);
        (void)q.doc.add_part(make_part("R", {0, -1000}, "R1"));
        (void)q.doc.add_wire({-100, -1000}, {100, -1000});
        q.tools.select(std::vector{part_ref(q.doc.sheet().parts[0].id), wire_ref(q.doc.sheet().wires[3].id)});
        CHECK(q.tools.duplicate());
        q.click(0, -3000);
        CHECK(q.doc.sheet().wires.size() == 5 && q.doc.undo_label() == "Paste 2 items");

        // Moving: a drag that ends on the wire cuts it, in the drag's step.
        sheet loose = line;
        (void)loose.parts.insert(10, make_part("R", {0, 1000}, "R1"));
        rig m(loose);
        m.drag(0, 1010, 0, 10);
        CHECK(m.steps() == 1 && m.doc.undo_label() == "Move R1");
        {
            const netlist nets = build_netlist(m.doc.sheet());
            CHECK(nets.net_of_pin(10, "1") != nets.net_of_pin(10, "2"));
            CHECK(m.doc.sheet().wires.size() == 4 && m.doc.find_wire(2) && m.doc.find_wire(3));
        }
        CHECK(m.tools.undo() && m.doc.sheet() == loose);
        // So does a nudge onto it.
        m.tools.select(part_ref(10));
        for (int i = 0; i < 10; ++i)
            CHECK(m.tools.nudge(0, -1));
        {
            const netlist nets = build_netlist(m.doc.sheet());
            CHECK(nets.net_of_pin(10, "1") != nets.net_of_pin(10, "2"));
            CHECK(m.steps() == 1 && m.doc.find_wire(2) && m.doc.find_wire(3));
        }

        // A wire that joined the pins before the move (a jumper) stays.
        sheet jumper;
        (void)jumper.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)jumper.wires.insert(2, wire{{-100, 0}, {100, 0}, {}});
        rig j(jumper);
        j.tools.select(part_ref(1));
        j.drag(0, 10, 300, 10);
        CHECK(*j.doc.find_wire(2) == (wire{{200, 0}, {400, 0}, {}}));

        // Drawing a wire tidies only what it drew: a wire continuing one
        // end to end joins it, the pair elsewhere stays.
        rig w(line);
        w.tools.set_tool(tools_t::tool::wire);
        w.click(500, 0);
        w.click(800, 0, modifiers::none, 2);
        CHECK(w.steps() == 1 && *w.doc.find_wire(1) == (wire{{-500, 0}, {800, 0}, {}}));
        CHECK(w.doc.find_wire(2) && w.doc.find_wire(3) && w.doc.sheet().wires.size() == 3);
    }

    void nudging_with_wires()
    {
        rig r(follow_sheet());
        const sheet before = r.doc.sheet();
        r.tools.select(part_ref(1));
        CHECK(r.tools.key_down(key::up).consumed);
        CHECK(*r.doc.find_wire(4) == (wire{{100, 100}, {400, 100}, {}}) && r.doc.sheet().wires.size() == 4);
        // Back down in the same step: the jog goes again, nothing is left.
        CHECK(r.tools.key_down(key::down).consumed);
        CHECK(r.doc.sheet() == before && r.steps() == 0);
        CHECK(r.tools.key_down(key::right).consumed && r.tools.key_down(key::right).consumed);
        CHECK(*r.doc.find_wire(3) == (wire{{-400, 0}, {100, 0}, {}}) && r.steps() == 1);
        CHECK(r.tools.undo() && r.doc.sheet() == before);

        // Alt and an arrow: detached.
        r.tools.select(part_ref(1));
        CHECK(r.tools.key_down(key::up, modifiers::alt).consumed);
        CHECK(r.doc.find_part(1)->position == point{0, 100} && r.doc.sheet().wires == before.wires);
        CHECK(r.tools.nudge(0, -1, true) && r.doc.sheet() == before);
    }

    void modifiers_and_rules()
    {
        // Ctrl+click toggles; a drag with Ctrl moves what is pressed (not
        // added to the selection) off the grid.
        rig r(small_sheet());
        r.click(600, 0);
        r.click(0, 10, modifiers::control);
        CHECK(selection_of(r.tools) == (std::vector{part_ref(2), part_ref(1)}));
        r.click(0, 10, modifiers::control);
        CHECK(selection_of(r.tools) == std::vector{part_ref(2)});
        r.drag(0, 10, 37, 10, modifiers::control);
        CHECK(selection_of(r.tools) == std::vector{part_ref(1)});
        CHECK(r.doc.find_part(1)->position == point{37, 0} && r.doc.find_part(2)->position == point{600, 0});
        // Shift still adds as the drag begins.
        r.drag(600, 10, 600, 110, modifiers::shift);
        CHECK(selection_of(r.tools) == (std::vector{part_ref(1), part_ref(2)}));

        // Ctrl while placing: off the grid.
        r.tools.begin_place_symbol("C");
        r.click(1013, 1007, modifiers::control);
        CHECK(r.doc.find_part(r.tools.selection()[0].id)->position == point{1013, 1007});
        CHECK(r.tools.hint().find("Ctrl") != std::string::npos);
        r.tools.set_tool(tools_t::tool::select);
        CHECK(r.tools.hint().find("Alt") != std::string::npos);

        // The rules: with pins connecting at wire ends only, a pin lying on
        // a wire is not attached to it, and a part dropped there does not
        // cut it.
        CHECK(r.tools.connectivity() == connectivity{});
        const std::uint64_t revision = r.tools.revision();
        r.tools.set_connectivity({.pins_inside_wires = false});
        CHECK(!r.tools.connectivity().pins_inside_wires && r.tools.revision() != revision);
        sheet line;
        (void)line.wires.insert(1, wire{{-500, 0}, {500, 0}, {}});
        rig o(line);
        o.tools.set_connectivity({.pins_inside_wires = false});
        o.tools.begin_place_symbol("R");
        o.click(0, 0);
        CHECK(o.doc.sheet().wires == line.wires && o.doc.undo_label() == "Place R1");
    }

    // A long random run: whatever the events, no transaction is left open
    // when no drag is, the selection holds only live items, and the history
    // still undoes back to the start.
    void random_events()
    {
        constexpr history::stack_limits unlimited{.max_steps = 0, .max_cost = 0, .merge_window = 4096};
        document docs[2] = {document(demo_ladder(), unlimited), document(small_sheet(), unlimited)};
        const sheet initial[2] = {demo_ladder(), small_sheet()};
        tools_t t;
        t.set_document(&docs[0]);
        int current = 0;

        std::mt19937 rng(20261122u);
        const auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
        const auto coordinate = [&] { return static_cast<float>(pick(2400) - 1200); };
        const modifiers mod_choices[] = {modifiers::none, modifiers::none, modifiers::none, modifiers::shift,
                                         modifiers::control, modifiers::alt};
        const mouse_button buttons[] = {mouse_button::left, mouse_button::left, mouse_button::left,
                                        mouse_button::right, mouse_button::middle};
        const key keys[] = {key::escape, key::enter,  key::backspace, key::del,  key::r,    key::x,  key::y,
                            key::space,  key::slash,  key::left,      key::right, key::up, key::down, key::a};
        const char *symbols[] = {"R", "C", "GND", "PWR", "V_DC", "Conn_01x04", "Mystery"};

        bool consistent = true;
        bool no_dead = true;
        std::size_t drags = 0;
        std::size_t wires_drawn = 0;
        for (int i = 0; i < 6000; ++i)
        {
            document &d = docs[current];
            const int what = pick(100);
            tools_t::pointer_input e{.at = at(coordinate(), coordinate()),
                                     .tolerance = static_cast<float>(pick(30)),
                                     .button = buttons[pick(5)],
                                     .mods = mod_choices[pick(6)],
                                     .clicks = pick(6) == 0 ? 2 : 1};
            if (what < 22)
                (void)t.pointer_down(e);
            else if (what < 50)
                (void)t.pointer_move(e);
            else if (what < 70)
                (void)t.pointer_up(e);
            else if (what < 72)
                (void)t.pointer_leave();
            else if (what < 82)
                (void)t.key_down(keys[pick(14)], pick(4) == 0 ? modifiers::shift : modifiers::none);
            else if (what < 85)
                t.set_tool(static_cast<tools_t::tool>(pick(6)));
            else if (what < 86)
                t.begin_place_symbol(symbols[pick(7)]);
            else
            {
                switch (pick(16))
                {
                case 0:
                    (void)t.copy();
                    break;
                case 1:
                    (void)t.cut();
                    break;
                case 2:
                    (void)t.paste();
                    break;
                case 3:
                    (void)t.duplicate();
                    break;
                case 4:
                    (void)t.delete_selection();
                    break;
                case 5:
                    (void)t.rotate_selection(pick(2) == 0);
                    break;
                case 6:
                    (void)t.mirror_selection(pick(2) == 0);
                    break;
                case 7:
                    (void)t.nudge(pick(3) - 1, pick(3) - 1);
                    break;
                case 8:
                    t.select_all();
                    break;
                case 9:
                case 10:
                    (void)t.undo();
                    break;
                case 11:
                    (void)t.redo();
                    break;
                case 12:
                    (void)t.cancel();
                    break;
                case 13:
                    t.set_grid(25 << pick(4));
                    break;
                case 14:
                    if (pick(4) == 0)
                    {
                        current = 1 - current;
                        t.set_document(&docs[current]);
                    }
                    break;
                default:
                    (void)t.toggle_wire_corner();
                    break;
                }
            }

            document &now = docs[current];
            drags += t.dragging() ? 1 : 0;
            wires_drawn += t.drawing_wire() ? 1 : 0;
            if (!t.dragging() && (now.in_transaction() || d.in_transaction()))
                consistent = false;
            if (t.dragging() && !now.in_transaction())
                consistent = false;
            for (const item_ref &item : t.selection())
                no_dead = no_dead && now.contains(item);
            std::vector<segment> out;
            t.append_overlay(out, draw_style{});
            (void)t.hint();
            (void)t.cursor();
        }
        CHECK(consistent);
        CHECK(no_dead);
        CHECK(drags > 0 && wires_drawn > 0);

        // Both histories still undo back to where they started.
        t.set_document(nullptr);
        for (int k = 0; k < 2; ++k)
        {
            CHECK(!docs[k].in_transaction());
            while (docs[k].undo())
            {
            }
            CHECK(docs[k].sheet() == initial[k]);
        }
    }

} // namespace

int main()
{
    clicking();
    cycling_and_double_click();
    dragging();
    drag_cancelled_and_returned();
    drag_snapping();
    wire_ends();
    rubber_band();
    demo_drag();
    wire_basics();
    wire_corners();
    wire_to_pin();
    wire_to_wire();
    wire_escapes();
    placing_parts();
    power_is_a_label();
    placing_labels();
    deleting_turning_mirroring();
    copy_paste();
    nudging();
    cancel_levels();
    undo_mid_gesture();
    switching_documents();
    no_document();
    out_of_order_and_underneath();
    overlay_states();
    wires_follow_a_drag();
    follow_kinds();
    turning_while_dragging();
    dragging_wire_ends_snaps();
    parts_dropped_on_wires_cut_them();
    nudging_with_wires();
    modifiers_and_rules();
    random_events();
    return volt::tests::result("schematic tools");
}
