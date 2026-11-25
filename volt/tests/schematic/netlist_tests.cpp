// The netlist: every connectivity rule and its negative (corners, crossings,
// T junctions, collinear overlaps, diagonals, pins on wires and on pins,
// and pins at wire ends only under the rules that say so),
// parts in all eight orientations, labels and power ports naming and
// joining nets, the problems, the lookups, determinism whatever order the
// tables are in, the demo ladder, and large sheets in good time.

#include "../math/check.hpp"

#include <schematic/demo.hpp>
#include <schematic/netlist.hpp>
#include <schematic/symbol.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace volt;
using namespace volt::schematic;

namespace
{
    // A sheet made item by item, ids from 1.
    struct builder
    {
        sheet s;
        item_id next = 1;

        item_id add_wire(point a, point b)
        {
            s.wires.insert(next, wire{a, b, {}});
            return next++;
        }

        item_id add_part(std::string symbol, point at, std::string reference, std::uint8_t rotation = 0,
                         bool mirrored = false, std::string value = {})
        {
            s.parts.insert(next, part{std::move(symbol), at, rotation, mirrored, std::move(reference), std::move(value),
                                      {}, {}, {}});
            return next++;
        }

        item_id add_label(label_kind kind, point at, std::string text)
        {
            s.labels.insert(next, label{kind, at, 0, std::move(text), {}});
            return next++;
        }
    };

    // The pin's point on the sheet, worked out here: the symbol's point
    // mirrored, turned and moved.
    point turn(point local, int rotation, bool mirrored)
    {
        int x = mirrored ? -local.x() : local.x();
        int y = local.y();
        for (int i = 0; i < rotation; ++i)
        {
            const int x0 = x;
            x = -y;
            y = x0;
        }
        return point{x, y};
    }

    bool same(const netlist &n, item_id a, item_id b)
    {
        const auto na = n.net_of_wire(a);
        const auto nb = n.net_of_wire(b);
        return na && nb && *na == *nb;
    }

    std::size_t count(const netlist &n, problem_kind kind)
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(n.problems, [&](const problem &p) { return p.kind == kind; }));
    }

    // Everything a netlist says, as text, for comparing two.
    std::string dump(const netlist &n)
    {
        std::string text;
        for (const net &each : n.nets)
        {
            text += std::format("{} named={} ground={} wires:", each.name, each.named, each.ground);
            for (const item_id w : each.wires)
                text += std::format(" {}", w);
            text += " pins:";
            for (const pin_ref &p : each.pins)
                text += std::format(" {}.{}", p.part, p.number);
            text += " labels:";
            for (const item_id l : each.labels)
                text += std::format(" {}", l);
            text += "\n";
        }
        for (const problem &p : n.problems)
        {
            text += std::format("{} {}:", to_string(p.level), p.message);
            for (const item_ref &r : p.items)
                text += std::format(" {}", r.id);
            text += "\n";
        }
        return text;
    }

    void empty()
    {
        const netlist n = build_netlist(sheet{});
        CHECK(n.nets.empty() && n.problems.empty());
        CHECK(!n.net_at({0, 0}) && !n.find("GND") && !n.net_of_wire(1) && !n.net_of_pin(1, "1"));
    }

    void corners_and_crossings()
    {
        // Two ends at one point: a corner.
        {
            builder b;
            const item_id w1 = b.add_wire({0, 0}, {200, 0});
            const item_id w2 = b.add_wire({200, 200}, {200, 0});
            const netlist n = build_netlist(b.s);
            CHECK(n.nets.size() == 1 && same(n, w1, w2));
            CHECK(n.problems.empty());
        }
        // Crossing, interior to interior: not connected.
        {
            builder b;
            const item_id h = b.add_wire({0, 0}, {400, 0});
            const item_id v = b.add_wire({200, -200}, {200, 200});
            const netlist n = build_netlist(b.s);
            CHECK(n.nets.size() == 2 && !same(n, h, v));
            // The crossing point is on both; either net will do.
            CHECK(n.net_at({200, 0}).has_value());
        }
        // A third wire ending at the crossing joins both.
        {
            builder b;
            const item_id h = b.add_wire({0, 0}, {400, 0});
            const item_id v = b.add_wire({200, -200}, {200, 200});
            const item_id d = b.add_wire({0, 200}, {200, 0});
            const netlist n = build_netlist(b.s);
            CHECK(n.nets.size() == 1 && same(n, h, v) && same(n, h, d));
        }
        // Diagonals crossing: not connected either.
        {
            builder b;
            const item_id a = b.add_wire({0, 0}, {200, 200});
            const item_id c = b.add_wire({0, 200}, {200, 0});
            const netlist n = build_netlist(b.s);
            CHECK(!same(n, a, c));
        }
        // A wire of no length is a point.
        {
            builder b;
            const item_id dot = b.add_wire({100, 0}, {100, 0});
            const item_id w = b.add_wire({0, 0}, {300, 0});
            const item_id far = b.add_wire({500, 0}, {500, 0});
            const netlist n = build_netlist(b.s);
            CHECK(same(n, dot, w) && !same(n, far, w) && n.nets.size() == 2);
        }
    }

    void t_junctions()
    {
        // An end inside a horizontal wire.
        {
            builder b;
            const item_id host = b.add_wire({0, 0}, {400, 0});
            const item_id t = b.add_wire({200, 0}, {200, 300});
            const item_id off = b.add_wire({300, 1}, {300, 300}); // one unit short
            const netlist n = build_netlist(b.s);
            CHECK(same(n, host, t) && !same(n, host, off) && n.nets.size() == 2);
        }
        // Inside a vertical wire, from either side, with the host's ends
        // the other way round.
        {
            builder b;
            const item_id host = b.add_wire({0, 400}, {0, 0});
            const item_id left = b.add_wire({-300, 100}, {0, 100});
            const item_id right = b.add_wire({0, 300}, {300, 300});
            const netlist n = build_netlist(b.s);
            CHECK(same(n, host, left) && same(n, host, right) && n.nets.size() == 1);
        }
        // Inside diagonal wires: only points exactly on them.
        {
            builder b;
            const item_id host = b.add_wire({0, 0}, {300, 300});
            const item_id on = b.add_wire({100, 100}, {100, -200});
            const item_id near = b.add_wire({200, 201}, {500, 201});
            const item_id slope = b.add_wire({1000, 0}, {1200, 100});
            const item_id on_slope = b.add_wire({1100, 50}, {1100, 400});
            const item_id off_slope = b.add_wire({1150, 76}, {1150, 400}); // (1150, 75) would be on it
            const netlist n = build_netlist(b.s);
            CHECK(same(n, host, on) && !same(n, host, near));
            CHECK(same(n, slope, on_slope) && !same(n, slope, off_slope));
        }
        // A wire end on a crossing of two others, and a T into each of two
        // wires that cross elsewhere.
        {
            builder b;
            const item_id h = b.add_wire({0, 0}, {1000, 0});
            const item_id v = b.add_wire({500, -500}, {500, 500});
            const item_id t1 = b.add_wire({200, 0}, {200, -300});
            const item_id t2 = b.add_wire({500, 300}, {800, 300});
            const netlist n = build_netlist(b.s);
            CHECK(same(n, h, t1) && same(n, v, t2) && !same(n, h, v) && n.nets.size() == 2);
        }
    }

    void collinear()
    {
        builder b;
        // Overlapping.
        const item_id a1 = b.add_wire({0, 0}, {300, 0});
        const item_id a2 = b.add_wire({200, 0}, {500, 0});
        // One inside the other, ends nowhere near the outer's.
        const item_id b1 = b.add_wire({0, 1000}, {1000, 1000});
        const item_id b2 = b.add_wire({600, 1000}, {300, 1000});
        // End to end.
        const item_id c1 = b.add_wire({0, 2000}, {300, 2000});
        const item_id c2 = b.add_wire({300, 2000}, {600, 2000});
        // On one line with a gap.
        const item_id d1 = b.add_wire({0, 3000}, {300, 3000});
        const item_id d2 = b.add_wire({400, 3000}, {600, 3000});
        // Parallel, one unit apart.
        const item_id e1 = b.add_wire({0, 4000}, {300, 4000});
        const item_id e2 = b.add_wire({100, 4001}, {200, 4001});
        // Overlapping diagonals, and vertical ones.
        const item_id f1 = b.add_wire({0, 5000}, {300, 5300});
        const item_id f2 = b.add_wire({400, 5400}, {200, 5200});
        const item_id g1 = b.add_wire({5000, 0}, {5000, 400});
        const item_id g2 = b.add_wire({5000, 600}, {5000, 100});
        // A chain of overlaps: the first and last overlap nothing but
        // their neighbour.
        const item_id h1 = b.add_wire({0, 6000}, {200, 6000});
        const item_id h2 = b.add_wire({100, 6000}, {400, 6000});
        const item_id h3 = b.add_wire({300, 6000}, {600, 6000});
        const netlist n = build_netlist(b.s);
        CHECK(same(n, a1, a2));
        CHECK(same(n, b1, b2));
        CHECK(same(n, c1, c2));
        CHECK(!same(n, d1, d2));
        CHECK(!same(n, e1, e2));
        CHECK(same(n, f1, f2));
        CHECK(same(n, g1, g2));
        CHECK(same(n, h1, h3) && same(n, h1, h2));
        CHECK(n.nets.size() == 10);
    }

    void pins()
    {
        builder b;
        // R1: a wire through pin 1 (the pin inside it), and one ending at pin 2.
        const item_id r1 = b.add_part("R", {0, 0}, "R1");
        const item_id through = b.add_wire({-100, -200}, {-100, 200});
        const item_id end = b.add_wire({100, 0}, {300, 0});
        // R2: a wire one unit off pin 1.
        const item_id r2 = b.add_part("R", {1000, 0}, "R2");
        const item_id off = b.add_wire({900, 1}, {900, 200});
        // R3 and R4: pin to pin, no wire.
        const item_id r3 = b.add_part("R", {2000, 0}, "R3");
        const item_id r4 = b.add_part("R", {2200, 0}, "R4");
        // R5 lying along a wire: both pins inside it, so it is shorted.
        const item_id r5 = b.add_part("R", {3000, 0}, "R5");
        const item_id along = b.add_wire({2800, 0}, {3200, 0});

        const netlist n = build_netlist(b.s);
        CHECK(n.net_of_pin(r1, "1") == n.net_of_wire(through));
        CHECK(n.net_of_pin(r1, "2") == n.net_of_wire(end));
        CHECK(n.net_of_pin(r1, "1") != n.net_of_pin(r1, "2"));
        CHECK(n.net_of_pin(r2, "1") != n.net_of_wire(off));
        CHECK(n.net_of_pin(r3, "2") == n.net_of_pin(r4, "1"));
        CHECK(n.net_of_pin(r3, "1") != n.net_of_pin(r3, "2"));
        CHECK(n.net_of_pin(r5, "1") == n.net_of_wire(along) && n.net_of_pin(r5, "2") == n.net_of_wire(along));

        const net &shared = n.nets[*n.net_of_pin(r3, "2")];
        CHECK(shared.pins.size() == 2 && shared.wires.empty());
        CHECK(shared.pins[0] == (pin_ref{r3, "2"}) && shared.pins[1] == (pin_ref{r4, "1"}));
        CHECK(shared.name == "Net-(R3-2)" && !shared.named);
        CHECK(n.nets[*n.net_of_wire(along)].pins.size() == 2);

        // Unconnected: both of R2's, R3's 1, R4's 2. A pin with wires that
        // lead nowhere: both of R1's.
        CHECK(count(n, problem_kind::unconnected_pin) == 4);
        CHECK(count(n, problem_kind::single_pin_net) == 2);
        CHECK(std::ranges::any_of(n.problems, [&](const problem &p) {
            return p.kind == problem_kind::unconnected_pin && p.message == "R2 pin 1 is not connected." &&
                   p.items == std::vector<item_ref>{{item_kind::part, r2}} && p.level == severity::warning;
        }));
        CHECK(std::ranges::any_of(n.problems, [&](const problem &p) {
            return p.kind == problem_kind::single_pin_net && p.items.size() == 2 && p.items[0].id == r1 &&
                   p.items[1] == item_ref{item_kind::wire, end};
        }));
        // A pin without a reference is named by its symbol.
        builder unnamed;
        const item_id u = unnamed.add_part("C", {0, 0}, "");
        const netlist m = build_netlist(unnamed.s);
        CHECK(m.nets.size() == 2 && m.nets[0].name == "Net-(C?-1)" && m.nets[0].pins.front().part == u);
    }

    // The same sheet under rules that connect pins at wire ends only: a
    // pin inside a wire goes by it; at an end, and pin to pin, it connects
    // as before; labels and T joins inside wires are not affected.
    void pins_at_wire_ends_only()
    {
        builder b;
        const item_id r1 = b.add_part("R", {0, 0}, "R1");
        const item_id through = b.add_wire({-100, -200}, {-100, 200});
        const item_id end = b.add_wire({100, 0}, {300, 0});
        const item_id r3 = b.add_part("R", {2000, 0}, "R3");
        const item_id r4 = b.add_part("R", {2200, 0}, "R4");
        const item_id r5 = b.add_part("R", {3000, 0}, "R5");
        const item_id along = b.add_wire({2800, 0}, {3200, 0});
        const item_id tee = b.add_wire({3000, 0}, {3000, -300}); // a wire's end inside `along`
        const item_id name = b.add_label(label_kind::net, {2850, 0}, "BUS"); // a label inside it

        const connectivity ends_only{.pins_inside_wires = false};
        const netlist n = build_netlist(b.s, ends_only);
        CHECK(n.net_of_pin(r1, "1") != n.net_of_wire(through));
        CHECK(n.net_of_pin(r1, "2") == n.net_of_wire(end));
        CHECK(n.net_of_pin(r3, "2") == n.net_of_pin(r4, "1"));
        CHECK(n.net_of_pin(r5, "1") != n.net_of_wire(along) && n.net_of_pin(r5, "2") != n.net_of_wire(along));
        CHECK(n.net_of_pin(r5, "1") != n.net_of_pin(r5, "2"));
        CHECK(same(n, along, tee) && n.net_of_label(name) == n.net_of_wire(along));
        // The pin's own net at its point; the wire's just beside it.
        CHECK(n.net_at({-100, 0}) == n.net_of_pin(r1, "1") && n.net_at({-100, 1}) == n.net_of_wire(through));
        // R5's pins touch nothing now, and neither does R1's pin 1.
        CHECK(count(n, problem_kind::unconnected_pin) == 5); // and R3's 1, R4's 2

        // The default rules, given or not, are the old ones.
        const netlist d = build_netlist(b.s);
        CHECK(dump(d) == dump(build_netlist(b.s, connectivity{})));
        CHECK(d.net_of_pin(r1, "1") == d.net_of_wire(through) && d.net_of_pin(r5, "1") == d.net_of_wire(along));
    }

    void orientations()
    {
        struct case_
        {
            std::string symbol;
            std::vector<std::pair<std::string, point>> pins; // number, symbol point
            std::vector<point> outward;                     // a step away from the body, per pin
        };
        const std::vector<case_> cases = {
            {"R", {{"1", {-100, 0}}, {"2", {100, 0}}}, {{-1, 0}, {1, 0}}},
            {"V_DC", {{"1", {0, 200}}, {"2", {0, -200}}}, {{0, 1}, {0, -1}}},
            {"Q_NPN", {{"1", {-200, 0}}, {"2", {100, 200}}, {"3", {100, -200}}}, {{-1, 0}, {0, 1}, {0, -1}}},
        };
        const point at{700, -300};
        for (const case_ &c : cases)
        {
            for (int rotation = 0; rotation < 4; ++rotation)
            {
                for (const bool mirrored : {false, true})
                {
                    builder b;
                    const item_id p = b.add_part(c.symbol, at, "X1", static_cast<std::uint8_t>(rotation), mirrored);
                    std::vector<item_id> leads;
                    for (std::size_t i = 0; i < c.pins.size(); ++i)
                    {
                        const point pin = turn(c.pins[i].second, rotation, mirrored) + at;
                        const point away = turn(c.outward[i], rotation, mirrored) * 300;
                        leads.push_back(b.add_wire(pin + away, pin));
                    }
                    const netlist n = build_netlist(b.s);
                    CHECK(n.nets.size() == c.pins.size() && n.problems.size() == c.pins.size());
                    for (std::size_t i = 0; i < c.pins.size(); ++i)
                    {
                        CHECK(n.net_of_pin(p, c.pins[i].first) == n.net_of_wire(leads[i]));
                        for (std::size_t j = 0; j < i; ++j)
                            CHECK(!same(n, leads[i], leads[j]));
                    }
                }
            }
        }
        // A few by hand: an upright resistor has pin 1 at the bottom, the
        // source mirrored and turned once has + on the left.
        CHECK(turn({-100, 0}, 1, false) == point{0, -100});
        CHECK(turn({0, 200}, 1, true) == point{-200, 0});
        CHECK(turn({100, 200}, 2, true) == point{100, -200});
    }

    void labels()
    {
        builder b;
        // Two distant wires labelled A, at an end and inside.
        const item_id a1 = b.add_wire({0, 0}, {400, 0});
        const item_id a2 = b.add_wire({5000, 0}, {5400, 0});
        const item_id la1 = b.add_label(label_kind::net, {0, 0}, "A");
        const item_id la2 = b.add_label(label_kind::net, {5200, 0}, "A");
        // B elsewhere.
        const item_id wb = b.add_wire({0, 1000}, {400, 1000});
        b.add_label(label_kind::net, {400, 1000}, "B");
        // C touching nothing (one unit off a wire's end).
        const item_id lc = b.add_label(label_kind::net, {0, 1001}, "C");
        // P on a pin with nothing else.
        const item_id r = b.add_part("R", {9000, 0}, "R1");
        const item_id lp = b.add_label(label_kind::net, {8900, 0}, "P");
        // A note on a wire means nothing.
        const item_id wn = b.add_wire({0, 2000}, {400, 2000});
        b.add_label(label_kind::note, {200, 2000}, "Z");

        const netlist n = build_netlist(b.s);
        CHECK(same(n, a1, a2) && !same(n, a1, wb));
        const net &a = n.nets[*n.net_of_wire(a1)];
        CHECK(a.name == "A" && a.named && !a.ground && a.labels == std::vector<item_id>{la1, la2});
        CHECK(n.find("A") == n.net_of_wire(a2) && n.find("B") == n.net_of_wire(wb));
        CHECK(n.net_of_label(la2) == n.net_of_wire(a1));
        CHECK(n.net_at({5300, 0}) == n.net_of_wire(a1) && n.net_at({5200, 0}) == n.net_of_wire(a1));

        // C is a net of a label alone.
        const auto c = n.find("C");
        CHECK(c && n.nets[*c].wires.empty() && n.nets[*c].pins.empty() && n.nets[*c].labels.size() == 1);
        CHECK(std::ranges::any_of(n.problems, [&](const problem &p) {
            return p.kind == problem_kind::label_unconnected && p.items == std::vector<item_ref>{{item_kind::label, lc}} &&
                   p.message == "Net label C touches no wire or pin.";
        }));
        CHECK(count(n, problem_kind::label_unconnected) == 1);

        // P names R1's pin 1, which is then a net of one pin.
        CHECK(n.net_of_pin(r, "1") == n.net_of_label(lp) && n.nets[*n.find("P")].named);
        CHECK(count(n, problem_kind::single_pin_net) == 1 && count(n, problem_kind::unconnected_pin) == 1);

        // The note is no label of a net.
        CHECK(!n.nets[*n.net_of_wire(wn)].named && n.nets[*n.net_of_wire(wn)].labels.empty());
        CHECK(!n.find("Z"));
        CHECK(n.nets[*n.net_of_wire(wn)].name == std::format("N{}", wn));
    }

    void power()
    {
        builder b;
        // +5V at two distant wires.
        const item_id p1 = b.add_wire({0, 0}, {400, 0});
        const item_id p2 = b.add_wire({5000, 0}, {5400, 0});
        b.add_label(label_kind::power, {400, 0}, "+5V");
        b.add_label(label_kind::power, {5000, 0}, "+5V");
        // A net label VCC on the same net: the power port's name wins.
        const item_id vcc = b.add_label(label_kind::net, {200, 0}, "VCC");
        // Ground.
        const item_id g = b.add_wire({0, -1000}, {400, -1000});
        b.add_label(label_kind::power, {0, -1000}, "GND");
        const item_id ag = b.add_wire({0, -2000}, {400, -2000});
        b.add_label(label_kind::power, {0, -2000}, "AGND");
        // Two net labels on one net: alphabetical.
        const item_id two = b.add_wire({0, 3000}, {400, 3000});
        b.add_label(label_kind::net, {0, 3000}, "beta");
        b.add_label(label_kind::net, {400, 3000}, "alpha");
        // A power port touching nothing.
        const item_id lonely = b.add_label(label_kind::power, {9000, 9000}, "+12V");
        // A pin straight on a power port: global, so no single-pin net.
        const item_id r = b.add_part("R", {7000, 0}, "R1");
        b.add_label(label_kind::power, {6900, 0}, "+5V");
        b.add_wire({7100, 0}, {7100, 500});
        b.add_label(label_kind::net, {7100, 500}, "X");
        // A net label and a power port of one name are one net.
        const item_id x = b.add_wire({0, 4000}, {400, 4000});
        b.add_label(label_kind::power, {400, 4000}, "X");

        const netlist n = build_netlist(b.s);
        const auto five = n.find("+5V");
        CHECK(five && n.net_of_wire(p1) == five && n.net_of_wire(p2) == five && n.net_of_pin(r, "1") == five);
        CHECK(!n.find("VCC") && n.net_of_label(vcc) == five);
        CHECK(n.nets[*five].named && !n.nets[*five].ground && n.nets[*five].labels.size() == 4);
        CHECK(n.nets[*n.net_of_wire(g)].name == "GND" && n.nets[*n.net_of_wire(g)].ground);
        CHECK(n.nets[*n.net_of_wire(ag)].name == "AGND" && n.nets[*n.net_of_wire(ag)].ground);
        CHECK(!same(n, g, ag)); // different grounds stay different nets
        CHECK(n.nets[*n.net_of_wire(two)].name == "alpha");
        CHECK(n.net_of_pin(r, "2") == n.net_of_wire(x) && n.find("X") == n.net_of_wire(x));

        // Several names: +5V and VCC, alpha and beta; X is one name twice.
        CHECK(count(n, problem_kind::conflicting_names) == 2);
        CHECK(std::ranges::any_of(n.problems, [&](const problem &p) {
            return p.kind == problem_kind::conflicting_names &&
                   p.message == "One net has the names +5V and VCC; it is called +5V." && p.items.size() == 4;
        }));
        CHECK(std::ranges::any_of(n.problems, [&](const problem &p) {
            return p.kind == problem_kind::label_unconnected && p.items.front().id == lonely &&
                   p.message == "Power port +12V touches no wire or pin.";
        }));
        // R1's pin 2 is on net X with nothing but a power port elsewhere:
        // global, not reported. Pin 1 likewise.
        CHECK(count(n, problem_kind::single_pin_net) == 0 && count(n, problem_kind::unconnected_pin) == 0);

        // A net label of a ground name is a ground too.
        builder c;
        const item_id w = c.add_wire({0, 0}, {100, 0});
        c.add_label(label_kind::net, {0, 0}, "gnd");
        CHECK(build_netlist(c.s).nets[0].ground && build_netlist(c.s).net_of_wire(w) == 0u);
    }

    void generated_names()
    {
        // A label taking the name an unnamed net would get: that net gets
        // the next one.
        builder b;
        const item_id r = b.add_part("R", {0, 0}, "R1");
        const item_id w = b.add_wire({100, 0}, {300, 0});
        b.add_label(label_kind::net, {300, 0}, "Net-(R1-1)");
        const netlist n = build_netlist(b.s);
        CHECK(n.nets[*n.net_of_wire(w)].name == "Net-(R1-1)" && n.nets[*n.net_of_wire(w)].named);
        CHECK(n.nets[*n.net_of_pin(r, "1")].name == "Net-(R1-1)_2");
        // Two parts of one reference.
        builder twins;
        twins.add_part("R", {0, 0}, "R1");
        twins.add_part("R", {1000, 0}, "R1");
        const netlist t = build_netlist(twins.s);
        std::set<std::string> names;
        for (const net &each : t.nets)
            names.insert(each.name);
        CHECK(names == std::set<std::string>{"Net-(R1-1)", "Net-(R1-2)", "Net-(R1-1)_2", "Net-(R1-2)_2"});
    }

    void lookups()
    {
        builder b;
        const item_id r = b.add_part("R", {0, 0}, "R1");
        const item_id w = b.add_wire({100, 0}, {500, 0});
        const item_id note = b.add_label(label_kind::note, {300, 0}, "hello");
        const netlist n = build_netlist(b.s);
        CHECK(n.net_of_pin(r, "2") == n.net_of_wire(w));
        CHECK(!n.net_of_pin(r, "3") && !n.net_of_pin(w, "1") && !n.net_of_wire(r) && !n.net_of_label(note));
        CHECK(n.net_of_pin(pin_ref{r, "1"}) == n.net_at({-100, 0}));
        CHECK(n.net_at({300, 0}) == n.net_of_wire(w) && n.net_at({500, 0}) == n.net_of_wire(w));
        CHECK(!n.net_at({300, 1}) && !n.net_at({600, 0}) && !n.net_at({0, 0}));
        CHECK(n.find(n.nets[0].name) == 0u);
    }

    // The ladder, with its wires and labels reordered, renumbered, or both.
    sheet shuffled(const sheet &s, std::uint32_t seed, bool renumber, std::map<item_id, item_id> &ids)
    {
        std::mt19937 random(seed);
        std::vector<item_id> all;
        for (const auto &e : s.parts)
            all.push_back(e.id);
        for (const auto &e : s.wires)
            all.push_back(e.id);
        for (const auto &e : s.labels)
            all.push_back(e.id);
        std::vector<item_id> fresh = all;
        if (renumber)
            std::ranges::shuffle(fresh, random);
        ids.clear();
        for (std::size_t i = 0; i < all.size(); ++i)
            ids[all[i]] = fresh[i] + 1000;

        const auto reorder = [&](const auto &table, auto &into) {
            std::vector<std::size_t> order(table.size());
            for (std::size_t i = 0; i < order.size(); ++i)
                order[i] = i;
            std::ranges::shuffle(order, random);
            for (const std::size_t i : order)
                into.insert(ids[table[i].id], table[i].record);
        };
        sheet out;
        reorder(s.parts, out.parts);
        reorder(s.wires, out.wires);
        reorder(s.labels, out.labels);
        return out;
    }

    sheet busy_sheet()
    {
        sheet s = demo_ladder();
        item_id next = s.max_id() + 1;
        // More to name: labels on the top rail, a second ground, a T, a
        // dangling wire, a lone label, a crossing.
        s.labels.insert(next++, label{label_kind::net, {-400, 200}, 0, "N1", {}});
        s.labels.insert(next++, label{label_kind::net, {-400, 200}, 0, "TAP", {}});
        s.labels.insert(next++, label{label_kind::power, {800, 200}, 0, "+5V", {}});
        s.wires.insert(next++, wire{{0, 200}, {0, 800}, {}});
        s.wires.insert(next++, wire{{-200, 600}, {200, 600}, {}});
        s.labels.insert(next++, label{label_kind::net, {3000, 3000}, 0, "LONE", {}});
        s.parts.insert(next++, part{"D", {2000, 0}, 1, true, "D1", "", "", {}, {}});
        s.wires.insert(next++, wire{{2000, 100}, {2000, 200}, {}});
        return s;
    }

    void determinism()
    {
        const sheet s = busy_sheet();
        const netlist n = build_netlist(s);
        CHECK(dump(build_netlist(s)) == dump(n));

        // Ids kept, tables reordered: the same netlist, name for name.
        for (std::uint32_t seed = 1; seed <= 5; ++seed)
        {
            std::map<item_id, item_id> ids;
            sheet reordered = shuffled(s, seed, false, ids);
            // shuffled() moves every id by 1000; put them back.
            sheet back;
            for (const auto &e : reordered.parts)
                back.parts.insert(e.id - 1000, e.record);
            for (const auto &e : reordered.wires)
                back.wires.insert(e.id - 1000, e.record);
            for (const auto &e : reordered.labels)
                back.labels.insert(e.id - 1000, e.record);
            CHECK(dump(build_netlist(back)) == dump(n));
        }

        // Ids changed too: the same partition, and the same names where a
        // label gave them.
        for (std::uint32_t seed = 10; seed <= 14; ++seed)
        {
            std::map<item_id, item_id> ids;
            const sheet other = shuffled(s, seed, true, ids);
            const netlist m = build_netlist(other);
            CHECK(m.nets.size() == n.nets.size() && m.problems.size() == n.problems.size());
            for (const net &each : n.nets)
            {
                std::optional<std::size_t> there;
                if (!each.wires.empty())
                    there = m.net_of_wire(ids[each.wires.front()]);
                else if (!each.pins.empty())
                    there = m.net_of_pin(ids[each.pins.front().part], each.pins.front().number);
                else
                    there = m.net_of_label(ids[each.labels.front()]);
                CHECK(there.has_value());
                if (!there)
                    continue;
                const net &mapped = m.nets[*there];
                std::set<item_id> wires_here;
                std::set<item_id> wires_there(mapped.wires.begin(), mapped.wires.end());
                for (const item_id w : each.wires)
                    wires_here.insert(ids[w]);
                std::set<std::pair<item_id, std::string>> pins_here;
                std::set<std::pair<item_id, std::string>> pins_there;
                for (const pin_ref &p : each.pins)
                    pins_here.emplace(ids[p.part], p.number);
                for (const pin_ref &p : mapped.pins)
                    pins_there.emplace(p.part, p.number);
                CHECK(wires_here == wires_there && pins_here == pins_there &&
                      each.labels.size() == mapped.labels.size());
                CHECK(each.named == mapped.named && each.ground == mapped.ground);
                if (each.named)
                    CHECK(each.name == mapped.name);
            }
        }
    }

    void ladder()
    {
        const sheet s = demo_ladder();
        const netlist n = build_netlist(s);
        CHECK(n.nets.size() == 6);
        CHECK(n.problems.empty());

        const auto id_of = [&](std::string_view reference) {
            for (const auto &e : s.parts)
                if (e.record.reference == reference)
                    return e.id;
            return item_id{0};
        };
        // In order of their smallest member: V1's two pins come first.
        CHECK(n.nets[0].name == "Net-(V1-1)" && !n.nets[0].named);
        CHECK(n.nets[1].name == "GND" && n.nets[1].named && n.nets[1].ground);
        CHECK(n.nets[2].name == "Net-(R1-2)");
        CHECK(n.net_of_pin(id_of("V1"), "2") == 1u && n.net_of_label(30) == 1u);
        CHECK(n.net_of_pin(id_of("V1"), "1") == n.net_of_pin(id_of("R1"), "1"));
        for (int stage = 0; stage < 4; ++stage)
        {
            const item_id series = id_of("R" + std::to_string(2 * stage + 1));
            const item_id shunt = id_of("R" + std::to_string(2 * stage + 2));
            CHECK(n.net_of_pin(series, "2") == n.net_of_pin(shunt, "2"));
            CHECK(n.net_of_pin(shunt, "1") == 1u);
            CHECK(n.nets[*n.net_of_pin(series, "2")].pins.size() == (stage < 3 ? 3u : 2u));
        }
        CHECK(n.nets[1].wires.size() == 8); // the rail, four pieces, and the four shunts' leads
        CHECK(n.nets[1].pins.size() == 5);
        std::set<std::size_t> distinct;
        for (const auto &e : s.wires)
            distinct.insert(*n.net_of_wire(e.id));
        CHECK(distinct.size() == 6);
    }

    void large()
    {
        // A lattice of 33 by 33 points, every neighbour joined by a wire of
        // its own: 2112 wires, one net.
        {
            builder b;
            const int side = 33;
            for (int i = 0; i < side; ++i)
                for (int j = 0; j + 1 < side; ++j)
                {
                    b.add_wire({i * 100, j * 100}, {i * 100, j * 100 + 100});
                    b.add_wire({j * 100, i * 100}, {j * 100 + 100, i * 100});
                }
            const auto start = std::chrono::steady_clock::now();
            const netlist n = build_netlist(b.s);
            const auto took = std::chrono::steady_clock::now() - start;
            CHECK(b.s.wires.size() == 2112 && n.nets.size() == 1 && n.nets[0].wires.size() == 2112);
            CHECK(took < std::chrono::seconds(2));
            std::printf("lattice of %zu wires: %lld ms\n", b.s.wires.size(),
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
        }
        // A thousand long wires each way, all crossing and none connected,
        // and a short one teed into each of the horizontal ones with a
        // resistor on its end: the case a pairwise test makes quadratic.
        {
            builder b;
            const int lines = 1000;
            std::vector<item_id> horizontal;
            for (int k = 0; k < lines; ++k)
            {
                horizontal.push_back(b.add_wire({-50, k * 100}, {lines * 100 + 50, k * 100}));
                b.add_wire({k * 100, -50}, {k * 100, lines * 100 + 50});
            }
            std::vector<item_id> parts;
            for (int k = 0; k < lines; ++k)
            {
                b.add_wire({-30 - (k % 2) * 10, k * 100}, {-30 - (k % 2) * 10, k * 100 + 50});
                parts.push_back(b.add_part("R", {-130 - (k % 2) * 10, k * 100 + 50}, std::format("R{}", k + 1)));
            }
            const auto start = std::chrono::steady_clock::now();
            const netlist n = build_netlist(b.s);
            const auto took = std::chrono::steady_clock::now() - start;
            CHECK(n.nets.size() == 2 * lines + lines); // and each resistor's free pin
            CHECK(n.net_of_pin(parts[17], "2") == n.net_of_wire(horizontal[17]));
            CHECK(n.nets[*n.net_of_wire(horizontal[17])].wires.size() == 2);
            CHECK(took < std::chrono::seconds(2));
            std::printf("%zu crossing wires, %zu parts: %lld ms\n", b.s.wires.size(), b.s.parts.size(),
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
        }
    }

} // namespace

int main()
{
    empty();
    corners_and_crossings();
    t_junctions();
    collinear();
    pins();
    pins_at_wire_ends_only();
    orientations();
    labels();
    power();
    generated_names();
    lookups();
    determinism();
    ladder();
    large();
    return volt::tests::result("schematic netlist");
}
