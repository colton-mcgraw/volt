// Wires that follow moved items (<schematic/follow.hpp>) and the document
// operations around them (<schematic/document.hpp>): stretching along,
// sliding across with the wires at the far end bending, jogs where they
// cannot, diagonal wires, wires moved whole, held points and the wires
// drawn from them, labels carried, T joins on moved wires, the
// connectivity rules; cutting the wires parts are dropped on, in every
// turn and every shape; scoped tidying; and the demo ladder moved part by
// part with its connections checked by the netlist each time.

#include "../math/check.hpp"

#include <schematic/demo.hpp>
#include <schematic/document.hpp>
#include <schematic/follow.hpp>
#include <schematic/netlist.hpp>
#include <schematic/symbol.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace volt;
using namespace volt::schematic;

namespace
{
    part make_part(std::string symbol, point at, std::string reference, std::uint8_t rotation = 0,
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

    label make_label(label_kind kind, point at, std::string text)
    {
        label l;
        l.kind = kind;
        l.position = at;
        l.text = std::move(text);
        return l;
    }

    item_ref part_ref(item_id id) { return {item_kind::part, id}; }
    item_ref wire_ref(item_id id) { return {item_kind::wire, id}; }
    item_ref label_ref(item_id id) { return {item_kind::label, id}; }

    wire w(point a, point b) { return wire{a, b, {}}; }

    std::vector<wire> wires_of(const sheet &s)
    {
        std::vector<wire> out;
        for (const auto &e : s.wires)
            out.push_back(e.record);
        return out;
    }

    bool tidy_and_orthogonal(const sheet &s)
    {
        for (const auto &e : s.wires)
        {
            if (e.record.a == e.record.b)
                return false;
            if (e.record.a.x() != e.record.b.x() && e.record.a.y() != e.record.b.y())
                return false;
        }
        return true;
    }

    // Every pin's net, as the pins on it, for comparing connections.
    std::vector<std::vector<pin_ref>> pin_nets(const sheet &s, const connectivity &rules = {})
    {
        const netlist nets = build_netlist(s, rules);
        std::vector<std::vector<pin_ref>> out;
        for (const auto &e : s.parts)
            for (const placed_pin &p : placed_pins(e.record))
                out.push_back(nets.nets[*nets.net_of_pin(e.id, p.pin->number)].pins);
        return out;
    }

    // The sheet after moving the items with their wires, as a nudge does;
    // undo must give back the start exactly, redo the result.
    sheet moved(const sheet &start, std::vector<item_ref> items, point delta, const connectivity &rules = {})
    {
        document d(start);
        CHECK(d.move_with_wires(items, delta, {}, rules));
        CHECK(d.history().steps().size() == 1 && !d.in_transaction());
        const sheet result = d.sheet();
        CHECK(d.undo() && d.sheet() == start);
        CHECK(d.redo() && d.sheet() == result);
        return result;
    }

    // ---- following ------------------------------------------------------------------

    // R1 (1) at the origin; a wire (2) from its pin 2 to (400, 0) and one
    // (3) from there down to (400, -300).
    sheet corner()
    {
        sheet s;
        (void)s.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)s.wires.insert(2, w({100, 0}, {400, 0}));
        (void)s.wires.insert(3, w({400, 0}, {400, -300}));
        return s;
    }

    void stretching_and_sliding()
    {
        const sheet s = corner();
        const auto nets = pin_nets(s);

        // Along: stretch and shrink.
        sheet r = moved(s, {part_ref(1)}, {-200, 0});
        CHECK(wires_of(r) == (std::vector{w({-100, 0}, {400, 0}), w({400, 0}, {400, -300})}));
        r = moved(s, {part_ref(1)}, {200, 0});
        CHECK(wires_of(r) == (std::vector{w({300, 0}, {400, 0}), w({400, 0}, {400, -300})}));
        // Onto the far end: the wire goes (no length), the corner wire now
        // ends on the pin.
        r = moved(s, {part_ref(1)}, {300, 0});
        CHECK(wires_of(r) == std::vector{w({400, 0}, {400, -300})} && pin_nets(r) == nets);

        // Across: wire 2 slides, wire 3 stretches or shrinks to take it.
        r = moved(s, {part_ref(1)}, {0, 200});
        CHECK(wires_of(r) == (std::vector{w({100, 200}, {400, 200}), w({400, 200}, {400, -300})}));
        r = moved(s, {part_ref(1)}, {0, -200});
        CHECK(wires_of(r) == (std::vector{w({100, -200}, {400, -200}), w({400, -200}, {400, -300})}));
        // To no length at most: wire 3 goes, wire 2 ends where it ended.
        r = moved(s, {part_ref(1)}, {0, -300});
        CHECK(wires_of(r) == std::vector{w({100, -300}, {400, -300})});
        // Past its other end: not a slide but a jog.
        r = moved(s, {part_ref(1)}, {0, -400});
        CHECK(wires_of(r) == (std::vector{w({100, 0}, {400, 0}), w({400, 0}, {400, -300}), w({100, 0}, {100, -400})}));
        CHECK(pin_nets(r) == nets && tidy_and_orthogonal(r));

        // Both at once: along the wire, then slid.
        r = moved(s, {part_ref(1)}, {100, 100});
        CHECK(wires_of(r) == (std::vector{w({200, 100}, {400, 100}), w({400, 100}, {400, -300})}));

        // A free far end slides with nothing to bend.
        sheet free;
        (void)free.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)free.wires.insert(2, w({100, 0}, {400, 0}));
        r = moved(free, {part_ref(1)}, {0, 100});
        CHECK(wires_of(r) == std::vector{w({100, 100}, {400, 100})});

        // Vertical wires the same way, turned.
        sheet upright;
        (void)upright.parts.insert(1, make_part("R", {0, 0}, "R1", 1)); // pins (0, -100) and (0, 100)
        (void)upright.wires.insert(2, w({0, 100}, {0, 400}));
        (void)upright.wires.insert(3, w({0, 400}, {300, 400}));
        r = moved(upright, {part_ref(1)}, {200, 100});
        CHECK(wires_of(r) == (std::vector{w({200, 200}, {200, 400}), w({200, 400}, {300, 400})}));
    }

    void jogs()
    {
        // What stops a slide, each in turn: then a jog at the pin.
        const auto jogged = [](const sheet &s, point delta) {
            const sheet r = moved(s, {part_ref(1)}, delta);
            const auto all = wires_of(r);
            CHECK(tidy_and_orthogonal(r) && pin_nets(r) == pin_nets(s));
            return all;
        };
        const std::vector<wire> jog_up{w({100, 0}, {400, 0}), w({400, 0}, {400, -300}), w({100, 0}, {100, 100})};

        sheet pin = corner(); // a pin at the far end
        (void)pin.parts.insert(9, make_part("R", {500, 0}, "R9"));
        std::vector<wire> r = jogged(pin, {0, 100});
        CHECK(r == jog_up);
        // Along and across: stretched along, then the jog.
        r = jogged(pin, {-100, 100});
        CHECK(r == (std::vector{w({0, 0}, {400, 0}), w({400, 0}, {400, -300}), w({0, 0}, {0, 100})}));

        sheet labelled = corner(); // a label at the far end
        (void)labelled.labels.insert(9, make_label(label_kind::net, {400, 0}, "X"));
        CHECK(jogged(labelled, {0, 100}) == jog_up);

        sheet tee = corner(); // the far end on the inside of another wire
        (void)tee.wires.insert(9, w({300, 0}, {600, 0}));
        r = jogged(tee, {0, 100});
        CHECK(r.size() == 4 && r[3] == w({100, 0}, {100, 100}));

        sheet onward = corner(); // a wire going on in a line at the far end
        (void)onward.wires.insert(9, w({400, 0}, {700, 0}));
        CHECK(jogged(onward, {0, 100}).size() == 4);

        sheet inside = corner(); // a T on the wire, which a slide would leave
        (void)inside.wires.insert(9, w({200, 0}, {200, -300}));
        r = jogged(inside, {0, 100});
        CHECK(r.size() == 4 && r[0] == w({100, 0}, {400, 0}));

        sheet reached = corner(); // a pin where the slid wire would lie
        (void)reached.parts.insert(9, make_part("R", {300, 200}, "R9", 1)); // pins (300, 100), (300, 300)
        r = jogged(reached, {0, 100});
        CHECK(r == jog_up);

        sheet landing = corner(); // the far end would land on a wire
        (void)landing.wires.insert(9, w({300, 100}, {600, 100}));
        r = jogged(landing, {0, 100});
        CHECK(r.size() == 4 && r[3] == w({100, 0}, {100, 100}));

        sheet passed = corner(); // the bent wire would pass over a wire's end
        (void)passed.wires.insert(9, w({400, 100}, {700, 100}));
        r = jogged(passed, {0, 200});
        CHECK(r.size() == 4 && r[3] == w({100, 0}, {100, 200}));

        // A wire bent by one slide is not bent again by another: the second
        // wire gets a jog.
        sheet shared;
        (void)shared.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)shared.parts.insert(5, make_part("R", {800, -300}, "R2"));
        (void)shared.wires.insert(2, w({100, 0}, {400, 0}));
        (void)shared.wires.insert(3, w({400, 0}, {400, -300}));
        (void)shared.wires.insert(4, w({400, -300}, {700, -300}));
        const sheet bent = moved(shared, {part_ref(1), part_ref(5)}, {0, 100});
        CHECK(wires_of(bent) == (std::vector{w({100, 100}, {400, 100}), w({400, 100}, {400, -300}),
                                             w({400, -300}, {700, -300}), w({700, -300}, {700, -200})}));
        CHECK(pin_nets(bent) == pin_nets(shared));

        // Both pins' wires slide, each bending its own corner wire.
        sheet twice;
        (void)twice.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)twice.wires.insert(2, w({100, 0}, {400, 0}));
        (void)twice.wires.insert(3, w({400, 0}, {400, -300}));
        (void)twice.wires.insert(4, w({-100, 0}, {-400, 0}));
        (void)twice.wires.insert(5, w({-400, 0}, {-400, -300}));
        const sheet both = moved(twice, {part_ref(1)}, {0, 100});
        CHECK(wires_of(both) == (std::vector{w({100, 100}, {400, 100}), w({400, 100}, {400, -300}),
                                             w({-100, 100}, {-400, 100}), w({-400, 100}, {-400, -300})}));

        // A jog that would run over the part's other pin goes round it: R1
        // raised on two jogs (as a drag up leaves it), then moved along its
        // axis past where its pin 2 was. Its pin 2's jog straight across
        // would pass over pin 1 and join the two nets (and the part dropped
        // on it would then cut it): it goes a square further up, across and
        // down instead. Pin 1's jog runs under that, crossing it, which
        // joins nothing.
        sheet raised;
        (void)raised.parts.insert(1, make_part("R", {0, 200}, "R1"));
        (void)raised.parts.insert(8, make_part("R", {-700, 0}, "R8"));
        (void)raised.parts.insert(9, make_part("R", {800, 0}, "R9"));
        (void)raised.wires.insert(2, w({-600, 0}, {-100, 0}));
        (void)raised.wires.insert(3, w({100, 0}, {700, 0}));
        (void)raised.wires.insert(4, w({-100, 0}, {-100, 200}));
        (void)raised.wires.insert(5, w({100, 0}, {100, 200}));
        (void)raised.labels.insert(6, make_label(label_kind::net, {400, 0}, "OUT"));
        const sheet round = moved(raised, {part_ref(1)}, {300, 0});
        CHECK(pin_nets(round) == pin_nets(raised));
        CHECK(tidy_and_orthogonal(round));
        const std::vector<wire> round_wires = wires_of(round);
        CHECK(std::ranges::find(round_wires, w({100, 300}, {400, 300})) != round_wires.end());
        CHECK(std::ranges::find(round_wires, w({400, 300}, {400, 200})) != round_wires.end());
        // And back the way it came: still nothing joined.
        CHECK(pin_nets(moved(round, {part_ref(1)}, {-300, 0})) == pin_nets(raised));
    }

    void other_shapes()
    {
        // A diagonal wire follows with its end.
        sheet diagonal;
        (void)diagonal.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)diagonal.wires.insert(2, w({100, 0}, {300, 200}));
        CHECK(wires_of(moved(diagonal, {part_ref(1)}, {0, 100})) == std::vector{w({100, 100}, {300, 200})});

        // A wire between two moved parts moves whole; so does one between a
        // moved part and a moved wire's end.
        sheet pair;
        (void)pair.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)pair.parts.insert(2, make_part("R", {500, 300}, "R2"));
        (void)pair.wires.insert(3, w({100, 0}, {400, 300})); // diagonal, attached at both ends
        (void)pair.wires.insert(4, w({-100, 0}, {-100, 300}));
        (void)pair.wires.insert(5, w({-100, 300}, {-400, 300}));
        sheet r = moved(pair, {part_ref(1), part_ref(2), wire_ref(5)}, {100, -100});
        CHECK(*r.wires.find(3) == w({200, -100}, {500, 200}));
        CHECK(*r.wires.find(4) == w({0, -100}, {0, 200}));
        CHECK(*r.wires.find(5) == w({0, 200}, {-300, 200}));

        // A moved label pulls the wire end on it; a label on a moved pin is
        // carried; a note is not.
        sheet labels;
        (void)labels.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)labels.labels.insert(2, make_label(label_kind::net, {100, 0}, "OUT"));
        (void)labels.labels.insert(3, make_label(label_kind::note, {-100, 0}, "a note"));
        (void)labels.labels.insert(4, make_label(label_kind::power, {0, 500}, "GND"));
        (void)labels.wires.insert(5, w({0, 500}, {0, 800}));
        r = moved(labels, {part_ref(1), label_ref(4)}, {0, -100});
        CHECK(r.labels.find(2)->position == point{100, -100} && r.labels.find(3)->position == point{-100, 0});
        CHECK(r.labels.find(4)->position == point{0, 400} && *r.wires.find(5) == w({0, 400}, {0, 800}));

        // A wire T'd onto a moved wire follows the point it is on.
        sheet bus;
        (void)bus.wires.insert(1, w({0, 0}, {1000, 0}));
        (void)bus.wires.insert(2, w({300, 0}, {300, -400}));
        (void)bus.wires.insert(3, w({300, -400}, {700, -400}));
        r = moved(bus, {wire_ref(1)}, {0, 100});
        CHECK(*r.wires.find(2) == w({300, 100}, {300, -400}) && *r.wires.find(3) == w({300, -400}, {700, -400}));
        r = moved(bus, {wire_ref(1)}, {100, 0}); // along the bus: the stub slides with it
        CHECK(*r.wires.find(2) == w({400, 0}, {400, -400}) && *r.wires.find(3) == w({400, -400}, {700, -400}));
    }

    void held_points()
    {
        // A pin on a pin that stays: a wire from the one that stays to the
        // one that moved, and the wire there stays with the pin that stays.
        sheet touching;
        (void)touching.parts.insert(1, make_part("R", {0, 0}, "R1"));
        (void)touching.parts.insert(2, make_part("R", {200, 0}, "R2"));
        (void)touching.wires.insert(3, w({100, 0}, {100, -300}));
        sheet r = moved(touching, {part_ref(1)}, {-100, 200});
        CHECK(*r.wires.find(3) == w({100, 0}, {100, -300}));
        // Across the vertical wire there (horizontal first), then up.
        CHECK(r.wires.size() == 3 && r.wires[1].record == w({100, 0}, {0, 0}));
        CHECK(r.wires[2].record == w({0, 0}, {0, 200}));
        CHECK(pin_nets(r) == pin_nets(touching));

        // A pin T'd into a wire that stays: held, unless the move keeps it
        // on the wire; under rules that connect pins at wire ends only, it
        // was not connected to begin with.
        sheet tee;
        (void)tee.parts.insert(1, make_part("R", {0, 100}, "R1", 1)); // pins (0, 0) and (0, 200)
        (void)tee.wires.insert(2, w({-500, 0}, {500, 0}));
        r = moved(tee, {part_ref(1)}, {0, 100});
        CHECK(*r.wires.find(2) == w({-500, 0}, {500, 0}) && r.wires.size() == 2);
        CHECK(r.wires[1].record == w({0, 0}, {0, 100}));
        CHECK(pin_nets(r) == pin_nets(tee));
        r = moved(tee, {part_ref(1)}, {200, 0}); // along the wire: still on it
        CHECK(wires_of(r) == wires_of(tee));
        const connectivity ends_only{.pins_inside_wires = false};
        document d(tee);
        CHECK(d.move_with_wires(std::vector{part_ref(1)}, {0, 100}, {}, ends_only));
        CHECK(wires_of(d.sheet()) == wires_of(tee));

        // A label alone moved off a wire leaves no wire behind.
        sheet named;
        (void)named.wires.insert(1, w({0, 0}, {500, 0}));
        (void)named.labels.insert(2, make_label(label_kind::net, {200, 0}, "X"));
        r = moved(named, {label_ref(2)}, {0, 300});
        CHECK(wires_of(r) == wires_of(named));
    }

    void plans()
    {
        // The plan alone: nothing for a move that touches nothing, the
        // sheet's order, the wires added last.
        const sheet s = corner();
        sheet after = s;
        CHECK(plan_follow(s, after, std::vector{part_ref(1)}).empty());
        part p = *s.parts.find(1);
        p.position = {0, 100};
        (void)after.parts.replace(1, p);
        const follow_plan plan = plan_follow(s, after, std::vector{part_ref(1)});
        CHECK(plan.wires.size() == 2 && plan.wires[0].first == 2 && plan.wires[1].first == 3);
        CHECK(plan.added.empty() && plan.labels.empty());
        CHECK(plan_follow(s, after, {}).empty()); // nothing said to have moved
        CHECK(plan_follow(s, after, std::vector{part_ref(77)}).empty());

        // follow_wires(): a step alone, "Move wires", returning the wires
        // changed and added.
        document d(s);
        const sheet start = d.sheet();
        CHECK(d.move(std::vector{part_ref(1)}, {0, 100}));
        const std::vector<item_id> touched = d.follow_wires(start, std::vector{part_ref(1)});
        CHECK(touched == (std::vector<item_id>{2, 3}) && d.undo_label() == "Move wires");
        CHECK(d.history().steps().size() == 2);
        CHECK(d.undo() && d.undo() && d.sheet() == start);
    }

    // The ladder, every part moved in turn, each way: connected as before,
    // orthogonal, nothing of no length; undo gives back the ladder.
    void ladder_moves()
    {
        const sheet ladder = demo_ladder();
        const auto nets = pin_nets(ladder);
        const point deltas[] = {{0, 100}, {0, -100}, {100, 0}, {-100, 0}, {100, 100}, {-100, 200}};
        std::size_t moves = 0;
        for (const auto &e : ladder.parts)
        {
            for (const point delta : deltas)
            {
                const sheet r = moved(ladder, {part_ref(e.id)}, delta);
                const bool same = pin_nets(r) == nets;
                const bool clean = tidy_and_orthogonal(r);
                CHECK(same && clean);
                if (!same || !clean)
                    std::fprintf(stderr, "  ladder: part %u moved by (%d, %d)\n", e.id, delta.x(), delta.y());
                ++moves;
            }
        }
        CHECK(moves == ladder.parts.size() * 6);
    }

    // ---- cutting -------------------------------------------------------------------

    void cutting()
    {
        // A resistor on a wire longer than it on both sides, in every turn
        // and mirrored: two wires, one ending at each pin, the pins on two
        // nets.
        for (int r = 0; r < 4; ++r)
        {
            for (const bool mirrored : {false, true})
            {
                document d;
                const bool upright = r % 2 == 1;
                (void)d.add_wire(upright ? w({0, -500}, {0, 500}) : w({-500, 0}, {500, 0}));
                const item_id id = d.add_part(make_part("R", {0, 0}, "R1", static_cast<std::uint8_t>(r), mirrored));
                const sheet before = d.sheet();
                CHECK(d.cut_wires_at_parts(std::vector{id}) == 1);
                CHECK(d.undo_label() == "Cut wires" && d.sheet().wires.size() == 2);
                const wire first = d.sheet().wires[0].record;
                const wire second = d.sheet().wires[1].record;
                CHECK(first == (upright ? w({0, -500}, {0, -100}) : w({-500, 0}, {-100, 0})));
                CHECK(second == (upright ? w({0, 100}, {0, 500}) : w({100, 0}, {500, 0})));
                const netlist nets = build_netlist(d.sheet());
                CHECK(nets.net_of_pin(id, "1") != nets.net_of_pin(id, "2"));
                CHECK(d.undo() && d.sheet() == before);
                const netlist shorted = build_netlist(before);
                CHECK(shorted.net_of_pin(id, "1") == shorted.net_of_pin(id, "2"));
            }
        }

        const auto cut = [](sheet s, std::vector<item_id> parts, const connectivity &rules = {},
                            std::vector<item_id> keep = {}) {
            document d(std::move(s));
            (void)d.cut_wires_at_parts(parts, rules, keep);
            return d.sheet();
        };
        sheet base;
        (void)base.parts.insert(1, make_part("R", {0, 0}, "R1"));

        // A wire ending at a pin and going through the other: what is past
        // the second pin is left.
        sheet s = base;
        (void)s.wires.insert(2, w({-100, 0}, {500, 0}));
        CHECK(wires_of(cut(s, {1})) == std::vector{w({100, 0}, {500, 0})});
        // Reversed: the same, the way it ran.
        s = base;
        (void)s.wires.insert(2, w({500, 0}, {-100, 0}));
        CHECK(wires_of(cut(s, {1})) == std::vector{w({500, 0}, {100, 0})});
        // From pin to pin: gone.
        s = base;
        (void)s.wires.insert(2, w({100, 0}, {-100, 0}));
        sheet r = cut(s, {1});
        CHECK(r.wires.empty());
        // Touching one pin only, ending on one, crossing the body: left alone.
        s = base;
        (void)s.wires.insert(2, w({100, 0}, {100, 300}));
        (void)s.wires.insert(3, w({-300, 0}, {0, 0}));
        (void)s.wires.insert(4, w({0, -300}, {0, 300}));
        (void)s.wires.insert(5, w({-300, 20}, {300, 20}));
        CHECK(cut(s, {1}) == s);

        // A connector column on a wire: every span between pins goes, what
        // is past the end pins stays, the middle pins are free.
        sheet column;
        (void)column.parts.insert(1, make_part("Conn_01x04", {0, 0}, "J1")); // pins x -200, y 100 .. -200
        (void)column.wires.insert(2, w({-200, 400}, {-200, -500}));
        r = cut(column, {1});
        CHECK(wires_of(r) == (std::vector{w({-200, 400}, {-200, 100}), w({-200, -200}, {-200, -500})}));
        {
            const netlist nets = build_netlist(r);
            CHECK(nets.net_of_pin(1, "1") != nets.net_of_pin(1, "4"));
            CHECK(nets.nets[*nets.net_of_pin(1, "2")].pins.size() == 1);
        }

        // A diagonal wire through two pins of a header.
        sheet header;
        (void)header.parts.insert(1, make_part("Conn_02x02", {0, 0}, "J1")); // 1 (-200, 0), 4 (200, -100)
        (void)header.wires.insert(2, w({-600, 100}, {600, -200}));
        CHECK(wires_of(cut(header, {1})) == (std::vector{w({-600, 100}, {-200, 0}), w({200, -100}, {600, -200})}));

        // Several wires and several parts at once; parts given twice or
        // unknown are no matter.
        sheet several = base;
        (void)several.parts.insert(2, make_part("C", {0, 500}, "C1"));
        (void)several.wires.insert(3, w({-300, 0}, {300, 0}));
        (void)several.wires.insert(4, w({-300, 500}, {300, 500}));
        (void)several.wires.insert(5, w({-200, 0}, {200, 0})); // overlapping the first
        document d(several);
        CHECK(d.cut_wires_at_parts(std::vector<item_id>{1, 2, 1, 99}) == 3);
        CHECK(d.history().steps().size() == 1);
        CHECK(d.sheet().wires.size() == 6);
        {
            const netlist nets = build_netlist(d.sheet());
            CHECK(nets.net_of_pin(1, "1") != nets.net_of_pin(1, "2"));
            CHECK(nets.net_of_pin(2, "1") != nets.net_of_pin(2, "2"));
        }
        // Nothing to cut: no step.
        CHECK(d.cut_wires_at_parts(std::vector<item_id>{1, 2}) == 0 && d.history().steps().size() == 1);

        // One wire through two parts: three pieces, the first keeping the
        // wire's id.
        sheet two = base;
        (void)two.parts.insert(2, make_part("R", {1000, 0}, "R2"));
        (void)two.wires.insert(3, w({-300, 0}, {1300, 0}));
        r = cut(two, {2, 1});
        CHECK(wires_of(r) == (std::vector{w({-300, 0}, {-100, 0}), w({100, 0}, {900, 0}), w({1100, 0}, {1300, 0})}));
        CHECK(*r.wires.find(3) == w({-300, 0}, {-100, 0}));
        {
            const netlist nets = build_netlist(r);
            CHECK(nets.net_of_pin(1, "2") == nets.net_of_pin(2, "1"));
            CHECK(nets.net_of_pin(1, "1") != nets.net_of_pin(1, "2") && nets.net_of_pin(2, "1") != nets.net_of_pin(2, "2"));
        }

        // Kept wires, and the rules: with pins connecting at wire ends
        // only, a wire through the pins does not touch them.
        s = base;
        (void)s.wires.insert(2, w({-300, 0}, {300, 0}));
        CHECK(cut(s, {1}, {}, {2}) == s);
        const connectivity ends_only{.pins_inside_wires = false};
        CHECK(cut(s, {1}, ends_only) == s);
        s = base;
        (void)s.wires.insert(2, w({-100, 0}, {100, 0}));
        CHECK(cut(s, {1}, ends_only).wires.empty());

        // In a transaction, it joins it.
        document t;
        t.begin("Place R1");
        (void)t.add_wire({-300, 0}, {300, 0});
        const item_id placed = t.add_part(make_part("R", {0, 0}, "R?"));
        CHECK(t.cut_wires_at_parts(std::vector{placed}) == 1);
        t.commit();
        CHECK(t.history().steps().size() == 1 && t.undo_label() == "Place R1");
        CHECK(t.reference_for(make_part("R", {}, "R?")) == "R2" && t.reference_for(make_part("C", {}, "")) == "C1");
        CHECK(t.reference_for(make_part("R", {}, "R7")) == "R7");
    }

    // ---- finishing a move, scoped tidying ---------------------------------------------

    void finishing()
    {
        // A drop on a wire cuts it; a wire that joined two of the part's
        // pins before stays; unrelated wires are not tidied.
        sheet s;
        (void)s.parts.insert(1, make_part("R", {0, 1000}, "R1"));
        (void)s.parts.insert(2, make_part("R", {2000, 1000}, "R2"));
        (void)s.wires.insert(3, w({-500, 0}, {500, 0}));
        (void)s.wires.insert(4, w({1900, 1000}, {2100, 1000})); // R2's jumper
        (void)s.wires.insert(5, w({-500, 3000}, {0, 3000}));
        (void)s.wires.insert(6, w({0, 3000}, {500, 3000}));
        document d(s);
        const sheet before = d.sheet();
        d.begin("Move 2 items");
        const std::vector items{part_ref(1), part_ref(2)};
        CHECK(d.move(items, {0, -1000}));
        (void)d.follow_wires(before, items);
        CHECK(d.finish_move(before, items));
        d.commit();
        CHECK(d.history().steps().size() == 1 && d.undo_label() == "Move 2 items");
        CHECK(d.sheet().wires.size() == 5 && d.find_wire(5) && d.find_wire(6));
        CHECK(*d.find_wire(4) == w({1900, 0}, {2100, 0}));
        const netlist nets = build_netlist(d.sheet());
        CHECK(nets.net_of_pin(1, "1") != nets.net_of_pin(1, "2"));
        CHECK(d.undo() && d.sheet() == before);

        // A move that leaves a point where two wires met in a line: there,
        // they are joined; the same pair elsewhere is not.
        sheet left;
        (void)left.parts.insert(1, make_part("R", {300, 0}, "R1")); // pin 1 at (200, 0)
        (void)left.wires.insert(2, w({0, 0}, {200, 0}));
        (void)left.wires.insert(3, w({200, 0}, {400, 0}));
        (void)left.wires.insert(4, w({0, 900}, {200, 900}));
        (void)left.wires.insert(5, w({200, 900}, {400, 900}));
        document l(left);
        const sheet start = l.sheet();
        l.begin("Move R1");
        CHECK(l.move(std::vector{part_ref(1)}, {0, 500}));
        CHECK(l.finish_move(start, std::vector{part_ref(1)}));
        l.commit();
        CHECK(*l.find_wire(2) == w({0, 0}, {400, 0}) && !l.find_wire(3));
        CHECK(l.find_wire(4) && l.find_wire(5));

        // move_with_wires(): nothing for no displacement, merging by key.
        document m(corner());
        CHECK(!m.move_with_wires(std::vector{part_ref(1)}, {}));
        CHECK(m.move_with_wires(std::vector{part_ref(1)}, {0, 100}, "nudge:1"));
        CHECK(m.move_with_wires(std::vector{part_ref(1)}, {0, 100}, "nudge:1"));
        CHECK(m.history().steps().size() == 1 && m.undo_label() == "Move R1");
        CHECK(*m.find_wire(2) == w({100, 200}, {400, 200}));
        CHECK(m.move_with_wires(std::vector{part_ref(1)}, {0, -200}, "nudge:1"));
        CHECK(m.history().steps().empty() && m.sheet() == corner());
    }

    void scoped_tidying()
    {
        // Two runs that could join, two duplicates, two wires of no length.
        sheet s;
        (void)s.wires.insert(1, w({0, 0}, {100, 0}));
        (void)s.wires.insert(2, w({100, 0}, {200, 0}));
        (void)s.wires.insert(3, w({0, 500}, {100, 500}));
        (void)s.wires.insert(4, w({100, 500}, {200, 500}));
        (void)s.wires.insert(5, w({0, 900}, {300, 900}));
        (void)s.wires.insert(6, w({300, 900}, {0, 900}));
        (void)s.wires.insert(7, w({50, 50}, {50, 50}));
        (void)s.wires.insert(8, w({70, 70}, {70, 70}));

        // The wires given: wire 2 joins wire 1 (which stays, out of the
        // scope or not); 7 goes, 8 stays; the duplicate in scope goes, the
        // first though out of it.
        document d(s);
        CHECK(d.tidy_wires(std::vector<item_id>{2, 5, 7, 99}) == 3);
        CHECK(*d.find_wire(1) == w({0, 0}, {200, 0}) && !d.find_wire(2));
        CHECK(d.find_wire(3) && d.find_wire(4) && !d.find_wire(5) && d.find_wire(6));
        CHECK(!d.find_wire(7) && d.find_wire(8));
        CHECK(d.undo_label() == "Tidy wires" && d.undo() && d.sheet() == s);

        // By points: what touches them, ends or insides.
        const point points[] = {{150, 500}, {300, 900}};
        CHECK(d.tidy_wires_at(points) == 2);
        CHECK(*d.find_wire(3) == w({0, 500}, {200, 500}) && !d.find_wire(4) && !d.find_wire(6));
        CHECK(d.find_wire(1) && d.find_wire(2) && d.find_wire(7));

        // Nothing in scope: nothing, no step.
        document e(s);
        CHECK(e.tidy_wires(std::vector<item_id>{}) == 0 && e.tidy_wires(std::vector<item_id>{42}) == 0);
        const point far[] = {{5000, 5000}};
        CHECK(e.tidy_wires_at(far) == 0 && !e.can_undo());
        // The whole sheet still does everything.
        CHECK(e.tidy_wires() == 5);
    }

} // namespace

int main()
{
    stretching_and_sliding();
    jogs();
    other_shapes();
    held_points();
    plans();
    ladder_moves();
    cutting();
    finishing();
    scoped_tidying();
    return volt::tests::result("schematic follow");
}
