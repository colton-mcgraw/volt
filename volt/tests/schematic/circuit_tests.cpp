// The circuit a sheet is to the simulation: the demo ladder solved on the
// CPU against the analytic answer and the hard-coded network it replaces; a
// divider; current sources; sources in chains, floating, fighting and
// shorted; values in engineering notation, good and bad; inductors as
// shorts; what is left out; floating nets; node numbering; the summary.

#include "../math/check.hpp"

#include <schematic/demo.hpp>
#include <schematic/netlist.hpp>
#include <schematic/symbol.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace volt;
using namespace volt::schematic;

namespace
{
    bool near(float a, float b, float tolerance = 1e-3f)
    {
        return std::abs(a - b) <= tolerance;
    }

    // Parts far apart, joined by labels on their pins.
    struct builder
    {
        sheet s;
        item_id next = 1;
        int x = 0;

        item_id add(std::string symbol, std::string reference, std::string value, std::string device = {})
        {
            s.parts.insert(next, part{std::move(symbol), {x, 0}, 0, false, std::move(reference), std::move(value),
                                      std::move(device), {}, {}});
            x += 2000;
            return next++;
        }

        point pin_at(item_id p, std::string_view number) const
        {
            for (const placed_pin &each : placed_pins(*s.parts.find(p)))
                if (each.pin->number == number)
                    return each.position;
            return {};
        }

        // A net label (or, for a ground or supply name, a power port) on
        // the pin.
        item_id tie(item_id p, std::string_view number, std::string name, bool power = false)
        {
            s.labels.insert(next, label{power || is_ground_name(name) ? label_kind::power : label_kind::net,
                                        pin_at(p, number), 0, std::move(name), {}});
            return next++;
        }

        // A two-terminal part between two nets.
        item_id between(std::string symbol, std::string reference, std::string value, std::string one,
                        std::string two)
        {
            const item_id p = add(std::move(symbol), std::move(reference), std::move(value));
            tie(p, "1", std::move(one));
            tie(p, "2", std::move(two));
            return p;
        }
    };

    struct solved
    {
        netlist nets;
        circuit c;
        std::vector<float> potentials;

        // The potential of the net of that name.
        float at(std::string_view name) const
        {
            const auto n = nets.find(name);
            if (!n || c.node_of_net[*n] < 0)
                return -1e9f;
            return potentials[static_cast<std::size_t>(c.node_of_net[*n])];
        }

        std::size_t count(problem_kind kind) const
        {
            return static_cast<std::size_t>(
                std::ranges::count_if(c.problems, [&](const problem &p) { return p.kind == kind; }));
        }
    };

    // Sweeps until nothing moves, as the GPU does.
    std::vector<float> relax_fully(const simulation::network &network)
    {
        const simulation::packed_network packed = simulation::pack(network);
        std::vector<float> from(packed.nodes.size(), 0.0f);
        std::vector<float> to(packed.nodes.size(), 0.0f);
        for (int sweep = 0; sweep < 200000; ++sweep)
        {
            simulation::relax(packed, from, to);
            float change = 0.0f;
            for (std::size_t i = 0; i < from.size(); ++i)
                change = std::max(change, std::abs(to[i] - from[i]));
            std::swap(from, to);
            if (sweep > 0 && change < 1e-7f)
                break;
        }
        return from;
    }

    solved solve(const sheet &s)
    {
        solved result;
        result.nets = build_netlist(s);
        result.c = build_circuit(s, result.nets);
        result.potentials = relax_fully(result.c.network);
        return result;
    }

    void empty()
    {
        const sheet s{};
        const circuit c = build_circuit(s, build_netlist(s));
        CHECK(c.network.nodes == 0 && c.network.branches.empty() && c.network.holds.empty());
        CHECK(c.node_of_net.empty() && c.wire_nodes.empty() && c.problems.empty());
        CHECK(c.low == 0.0f && c.high == 0.0f && c.summary == "Nothing to simulate");
    }

    // The ladder as <app/demo.hpp> has it, hard-coded: nodes 0 to 4 along
    // the top, 5 ground.
    simulation::network hard_coded_ladder()
    {
        simulation::network network;
        network.nodes = 6;
        network.holds = {{0, 5.0f}, {5, 0.0f}};
        for (std::uint32_t stage = 0; stage < 4; ++stage)
        {
            network.branches.push_back({stage, stage + 1, 1.0f / 100.0f});
            network.branches.push_back({stage + 1, 5, 1.0f / 220.0f});
        }
        return network;
    }

    void ladder()
    {
        const sheet s = demo_ladder();
        const solved r = solve(s);
        const circuit &c = r.c;
        CHECK(c.problems.empty());
        CHECK(c.network.nodes == 6 && c.network.branches.size() == 8 && c.network.holds.size() == 2 &&
              c.network.sources.empty());
        CHECK(c.summary == "6 nodes, 8 resistors, 1 source");
        CHECK(c.low == 0.0f && c.high == 5.0f);
        CHECK(c.wire_nodes.size() == 20);
        for (const auto &[id, node] : c.wire_nodes)
            CHECK(node >= 0 && node < 6);

        // Ground is node 0; the rest follow the nets.
        const auto ground = r.nets.find("GND");
        CHECK(ground && c.node_of_net[*ground] == 0);
        CHECK(c.network.holds[0].node == 0 && c.network.holds[0].potential == 0.0f);

        // The answer by hand: from the far end, each node sees 220 in
        // parallel with 100 plus what the next one sees.
        double seen[5] = {};
        seen[4] = 220.0;
        for (int k = 3; k >= 1; --k)
        {
            const double down = 100.0 + seen[k + 1];
            seen[k] = 220.0 * down / (220.0 + down);
        }
        double expected[5] = {5.0};
        for (int k = 1; k <= 4; ++k)
            expected[k] = expected[k - 1] * seen[k] / (100.0 + seen[k]);

        // Node k of the hard-coded ladder is the net of V1's + (k = 0) or of
        // the series resistor R(2k-1)'s pin 2.
        const std::vector<float> reference = relax_fully(hard_coded_ladder());
        const auto id_of = [&](std::string_view name) {
            for (const auto &e : s.parts)
                if (e.record.reference == name)
                    return e.id;
            return item_id{0};
        };
        for (int k = 0; k <= 4; ++k)
        {
            const auto n = k == 0 ? r.nets.net_of_pin(id_of("V1"), "1")
                                  : r.nets.net_of_pin(id_of("R" + std::to_string(2 * k - 1)), "2");
            CHECK(n.has_value());
            if (!n)
                continue;
            const float v = r.potentials[static_cast<std::size_t>(c.node_of_net[*n])];
            CHECK(near(v, static_cast<float>(expected[k])));
            CHECK(near(v, reference[static_cast<std::size_t>(k)]));
        }
        CHECK(near(r.potentials[0], 0.0f) && near(reference[5], 0.0f));
        // Every wire's node is its net's.
        for (const auto &e : s.wires)
            CHECK(c.wire_nodes.at(e.id) == c.node_of_net[*r.nets.net_of_wire(e.id)]);
    }

    void divider()
    {
        builder b;
        b.between("V_DC", "V1", "10", "IN", "GND");
        b.between("R", "R1", "100", "IN", "OUT");
        b.between("R", "R2", "300", "OUT", "GND");
        const solved r = solve(b.s);
        CHECK(r.c.problems.empty());
        CHECK(r.c.summary == "3 nodes, 2 resistors, 1 source");
        CHECK(near(r.at("IN"), 10.0f) && near(r.at("OUT"), 7.5f) && near(r.at("GND"), 0.0f));
        CHECK(r.c.low == 0.0f && r.c.high == 10.0f);
    }

    void current_sources()
    {
        // 2 mA out of pin 1, through 1 kOhm to ground: +2 V.
        {
            builder b;
            b.between("I_DC", "I1", "2mA", "OUT", "GND");
            b.between("R", "R1", "1k", "OUT", "GND");
            const solved r = solve(b.s);
            CHECK(r.c.problems.empty() && r.c.network.sources.size() == 2);
            CHECK(near(r.at("OUT"), 2.0f));
            CHECK(r.c.summary == "2 nodes, 1 resistor, 1 source");
        }
        // Turned round: -2 V.
        {
            builder b;
            b.between("I_DC", "I1", "0.002", "GND", "OUT");
            b.between("R", "R1", "1k", "OUT", "GND");
            const solved r = solve(b.s);
            CHECK(near(r.at("OUT"), -2.0f));
        }
    }

    void source_chains()
    {
        builder b;
        b.between("V_DC", "V1", "5", "A", "GND");
        b.between("V_DC", "V2", "3V", "B", "A");  // on top of V1
        b.between("V_DC", "V3", "2", "GND", "C"); // + on ground: C below it
        b.between("R", "R1", "1k", "B", "C");
        const solved r = solve(b.s);
        CHECK(r.c.problems.empty());
        CHECK(r.c.network.holds.size() == 4);
        CHECK(near(r.at("A"), 5.0f) && near(r.at("B"), 8.0f) && near(r.at("C"), -2.0f));
        CHECK(r.c.low == -2.0f && r.c.high == 8.0f);
        CHECK(r.c.summary == "4 nodes, 1 resistor, 3 sources");
    }

    void floating_sources()
    {
        // With a ground: a source off on its own is not supported.
        {
            builder b;
            b.between("V_DC", "V1", "5", "A", "GND");
            b.between("R", "R1", "1k", "A", "GND");
            const item_id v2 = b.between("V_DC", "V2", "3", "X", "Y");
            b.between("R", "R2", "1k", "X", "Y");
            const solved r = solve(b.s);
            CHECK(r.count(problem_kind::floating_source) == 1);
            const auto it = std::ranges::find(r.c.problems, problem_kind::floating_source, &problem::kind);
            CHECK(it != r.c.problems.end() && it->level == severity::error &&
                  it->items == std::vector<item_ref>{{item_kind::part, v2}});
            // And X and Y float.
            CHECK(r.count(problem_kind::floating_net) == 2);
            CHECK(r.c.network.holds.size() == 2 && near(r.at("A"), 5.0f));
            CHECK(r.c.summary == "4 nodes, 2 resistors, 1 source");
        }
        // No ground at all: each separate circuit's first source sets its
        // "-" as 0 V. A source joined to one by a resistor only is still
        // floating.
        {
            builder b;
            const item_id v1 = b.between("V_DC", "V1", "5", "P", "M");
            b.between("R", "R1", "1k", "P", "M");
            b.between("V_DC", "V2", "9", "Q", "N");
            b.between("R", "R2", "1k", "Q", "N");
            const item_id v3 = b.between("V_DC", "V3", "1", "S", "T");
            b.between("R", "R3", "1k", "S", "P");
            const solved r = solve(b.s);
            CHECK(r.count(problem_kind::reference_chosen) == 2);
            CHECK(r.count(problem_kind::floating_source) == 1);
            const auto chosen = std::ranges::find(r.c.problems, problem_kind::reference_chosen, &problem::kind);
            CHECK(chosen != r.c.problems.end() && chosen->level == severity::warning &&
                  chosen->items.front().id == v1 &&
                  chosen->message == "There is no ground, so net M (V1's - terminal) is taken as 0 V.");
            const auto floating = std::ranges::find(r.c.problems, problem_kind::floating_source, &problem::kind);
            CHECK(floating != r.c.problems.end() && floating->items.front().id == v3);
            CHECK(near(r.at("M"), 0.0f) && near(r.at("P"), 5.0f) && near(r.at("N"), 0.0f) && near(r.at("Q"), 9.0f));
            CHECK(near(r.at("S"), 5.0f)); // through R3, no current
        }
    }

    void fighting_sources()
    {
        builder b;
        const item_id v1 = b.between("V_DC", "V1", "5", "A", "GND");
        const item_id v2 = b.between("V_DC", "V2", "3", "A", "GND");
        b.between("V_DC", "V3", "5V", "A", "GND"); // agrees: fine
        const item_id v4 = b.between("V_DC", "V4", "1", "S", "S");
        b.between("V_DC", "V5", "0", "S", "S"); // shorted, but at 0 V: harmless
        b.between("R", "R1", "1k", "A", "S");
        b.between("R", "R2", "1k", "S", "GND");
        const solved r = solve(b.s);
        CHECK(r.count(problem_kind::conflicting_sources) == 1 && r.count(problem_kind::shorted_source) == 1);
        const auto fight = std::ranges::find(r.c.problems, problem_kind::conflicting_sources, &problem::kind);
        CHECK(fight != r.c.problems.end() && fight->level == severity::error &&
              fight->items == (std::vector<item_ref>{{item_kind::part, v2}, {item_kind::part, v1}}));
        CHECK(fight != r.c.problems.end() &&
              fight->message == "V2 would put 3V across nets A and GND, which are at 5V already (set by V1), so it "
                                "is left out.");
        const auto shorted = std::ranges::find(r.c.problems, problem_kind::shorted_source, &problem::kind);
        CHECK(shorted != r.c.problems.end() && shorted->items.front().id == v4);
        CHECK(near(r.at("A"), 5.0f) && near(r.at("S"), 2.5f));
        CHECK(r.c.summary == "3 nodes, 2 resistors, 2 sources");
    }

    // The conductance of the branch from the net named `name`, or 0.
    float conductance_from(const solved &r, std::string_view name)
    {
        const auto n = r.nets.find(name);
        if (!n)
            return 0.0f;
        const auto node = static_cast<std::uint32_t>(r.c.node_of_net[*n]);
        for (const simulation::network::branch &b : r.c.network.branches)
            if (b.a == node || b.b == node)
                return b.conductance;
        return 0.0f;
    }

    void values()
    {
        builder b;
        b.between("V_DC", "V1", "1", "A", "GND");
        // Good ones, each from a net of its own to A.
        const std::vector<std::pair<std::string, double>> good = {
            {"4k7", 4700.0},   {"4.7k", 4700.0}, {"100R", 100.0}, {"10kOhm", 1e4},      {"1 M", 1e6},
            {"2.2k\xCE\xA9", 2200.0}, {"4R7", 4.7}, {"1e3", 1e3},   {"33 ohms", 33.0}, {"0.5", 0.5},
        };
        for (std::size_t i = 0; i < good.size(); ++i)
            b.between("R", "R" + std::to_string(i + 1), good[i].first, "G" + std::to_string(i), "A");
        // Bad ones.
        const std::vector<std::string> bad = {"", "abc", "-5", "0", "10 kV", "1k2k"};
        std::vector<item_id> bad_ids;
        for (std::size_t i = 0; i < bad.size(); ++i)
            bad_ids.push_back(b.between("R", "R" + std::to_string(100 + i), bad[i], "B" + std::to_string(i), "A"));
        const item_id five = b.between("V_DC", "V2", "five", "Z", "GND");

        const solved r = solve(b.s);
        CHECK(r.c.network.branches.size() == good.size());
        for (std::size_t i = 0; i < good.size(); ++i)
        {
            const float g = conductance_from(r, "G" + std::to_string(i));
            CHECK(std::abs(g - static_cast<float>(1.0 / good[i].second)) <= 1e-6f * g);
        }
        CHECK(r.count(problem_kind::bad_value) == bad.size() + 1);
        for (const item_id id : bad_ids)
            CHECK(std::ranges::any_of(r.c.problems, [&](const problem &p) {
                return p.kind == problem_kind::bad_value && p.level == severity::error &&
                       p.items == std::vector<item_ref>{{item_kind::part, id}};
            }));
        CHECK(std::ranges::any_of(r.c.problems, [&](const problem &p) {
            return p.message == "R100 has no value, so it is left out.";
        }));
        CHECK(std::ranges::any_of(r.c.problems, [&](const problem &p) {
            return p.message == "R101's value \"abc\" is not a resistance, so it is left out.";
        }));
        CHECK(std::ranges::any_of(r.c.problems, [&](const problem &p) {
            return p.message == "R103's resistance must be more than zero, so it is left out.";
        }));
        CHECK(std::ranges::any_of(r.c.problems, [&](const problem &p) {
            return !p.items.empty() && p.items.front().id == five && p.message == "V2's value \"five\" is not a voltage, so it is left out.";
        }));
    }

    void inductors()
    {
        // L1 joins A and B: one node.
        {
            builder b;
            b.between("V_DC", "V1", "5", "A", "GND");
            const item_id l1 = b.between("L", "L1", "10u", "A", "B");
            b.between("R", "R1", "1k", "B", "GND");
            const solved r = solve(b.s);
            CHECK(r.c.node_of_net[*r.nets.find("A")] == r.c.node_of_net[*r.nets.find("B")]);
            CHECK(near(r.at("B"), 5.0f));
            CHECK(r.c.problems.size() == 1 && r.c.problems[0].kind == problem_kind::inductor_short &&
                  r.c.problems[0].level == severity::info && r.c.problems[0].items.front().id == l1 &&
                  r.c.problems[0].message == "L1 is a short at DC: the nets at either end are one node.");
            CHECK(r.c.summary == "2 nodes, 1 resistor, 1 inductor, 1 source");
        }
        // Across a source: the source is shorted.
        {
            builder b;
            b.between("V_DC", "V1", "5", "A", "GND");
            b.between("L", "L1", "", "A", "GND");
            const solved r = solve(b.s);
            CHECK(r.count(problem_kind::shorted_source) == 1 && r.c.network.nodes == 1);
        }
    }

    void left_out()
    {
        builder b;
        b.between("V_DC", "V1", "5", "A", "GND");
        b.between("R", "R1", "1k", "A", "GND");
        const item_id d1 = b.between("D", "D1", "", "A", "K1");
        const item_id led = b.between("LED", "LED1", "", "K1", "GND");
        const item_id d2 = b.between("D", "D2", "", "A", "K2");
        b.between("C", "C1", "100n", "A", "GND");
        const item_id j1 = b.add("Conn_01x02", "J1", "");
        b.tie(j1, "1", "A");
        const solved r = solve(b.s);
        CHECK(r.c.problems.size() == 3);
        CHECK(r.c.problems.size() == 3 && r.c.problems[0].kind == problem_kind::not_simulated &&
              r.c.problems[0].items == (std::vector<item_ref>{{item_kind::part, d1}, {item_kind::part, d2}}) &&
              r.c.problems[0].message == "D1 and D2 (diode) are not simulated yet, so they are left out.");
        CHECK(r.c.problems.size() == 3 && r.c.problems[1].items.front().id == led &&
              r.c.problems[1].message == "LED1 (led) is not simulated yet, so it is left out.");
        CHECK(r.c.problems.size() == 3 && r.c.problems[2].items.front().id == j1 &&
              r.c.problems[2].level == severity::info &&
              r.c.problems[2].message == "J1 has no simulation model, so it is left out.");
        // K1 and K2 still have nodes, but nothing simulated touches them:
        // not reported as floating.
        CHECK(r.c.node_of_net[*r.nets.find("K1")] >= 0 && r.count(problem_kind::floating_net) == 0);
        CHECK(r.c.summary == "5 nodes, 1 resistor, 1 source");
    }

    void floating_nets()
    {
        // A ground, and a resistor off on its own.
        {
            builder b;
            b.between("V_DC", "V1", "5", "A", "GND");
            b.between("R", "R1", "1k", "A", "GND");
            b.between("R", "R2", "1k", "X", "Y");
            const solved r = solve(b.s);
            CHECK(r.count(problem_kind::floating_net) == 2);
            CHECK(std::ranges::any_of(r.c.problems, [](const problem &p) {
                return p.message == "Net X has no path to ground or a voltage source, so its potential is undefined.";
            }));
        }
        // Nothing held at all: said once.
        {
            builder b;
            b.between("R", "R1", "1k", "X", "Y");
            b.between("I_DC", "I1", "1m", "X", "Y");
            const solved r = solve(b.s);
            CHECK(r.c.problems.size() == 1 && r.c.problems[0].kind == problem_kind::nothing_held);
            CHECK(r.c.network.holds.empty() && r.c.low == 0.0f && r.c.high == 0.0f);
        }
    }

    void devices_and_nodes()
    {
        builder b;
        // No device given: the symbol's. A device given: that one, on
        // whatever symbol, by its pins 1 and 2.
        b.between("V_DC", "V1", "5", "A", "GND");
        b.between("R", "R1", "1k", "A", "GND");
        const item_id x1 = b.add("C", "X1", "1k", "resistor");
        b.tie(x1, "1", "A");
        b.tie(x1, "2", "B");
        const item_id j = b.add("Conn_01x01", "J1", "1k", "resistor");
        b.tie(j, "1", "A");
        // Two grounds of different names: one node, 0.
        const item_id r3 = b.between("R", "R3", "1k", "B", "AGND");
        // A label alone: a net with no node.
        b.s.labels.insert(b.next++, label{label_kind::net, {-5000, -5000}, 0, "ALONE", {}});
        // A wire on its own: a node.
        b.s.wires.insert(b.next, wire{{-6000, 0}, {-5000, 0}, {}});
        const item_id lone_wire = b.next++;

        const solved r = solve(b.s);
        CHECK(r.c.network.branches.size() == 3); // R1, X1, R3; J1 has no pin 2
        CHECK(r.count(problem_kind::missing_pin) == 1);
        CHECK(std::ranges::any_of(r.c.problems, [&](const problem &p) {
            return p.kind == problem_kind::missing_pin && p.items.front().id == j &&
                   p.message == "J1 (resistor) has no pin 2, so it is left out.";
        }));
        const auto agnd = r.nets.net_of_pin(r3, "2");
        CHECK(agnd && r.nets.nets[*agnd].ground && r.c.node_of_net[*agnd] == 0 &&
              r.c.node_of_net[*r.nets.find("GND")] == 0);
        CHECK(near(r.at("B"), 2.5f));
        CHECK(r.c.node_of_net[*r.nets.find("ALONE")] == -1);
        CHECK(r.c.wire_nodes.at(lone_wire) > 0);
        // Numbered in net order after ground, without gaps.
        std::int32_t last = 0;
        for (std::size_t n = 0; n < r.nets.nets.size(); ++n)
        {
            const std::int32_t node = r.c.node_of_net[n];
            if (node <= 0)
                continue;
            CHECK(node <= last + 1);
            last = std::max(last, node);
        }
        CHECK(static_cast<std::uint32_t>(last + 1) == r.c.network.nodes);
    }

} // namespace

int main()
{
    empty();
    ladder();
    divider();
    current_sources();
    source_chains();
    floating_sources();
    fighting_sources();
    values();
    inductors();
    left_out();
    floating_nets();
    devices_and_nodes();
    return volt::tests::result("schematic circuit");
}
